// ---------------------------------------------------------------------------
// WuwaUID v2 — Wuthering Waves UID hider, self-calibrating
//
// A ReShade add-on for DirectX 12 that hides the on-screen player UID by
// suppressing the draw commands that render it.
//
// WHAT CHANGED FROM v1
//
// v1 baked one fingerprint into the binary:
//
//     { ps 0x5B44B3683F03BAE3, count 156, first 0, voff 0, indexed 1, ibo 1747200 }
//
// The `ibo` field is the offset of the UID's slice inside the game's shared UI
// index pool. It was measured once, on one game build, and it does not hold:
// three separate sessions measured 1747200, 94976 and 16765184. The offset is
// not a machine property and not even a per-launch allocation address — it is
// wherever the current build happens to place the UID inside a pool it fills
// at runtime. There is nothing stable to bake, so v1's rule went stale and the
// UID came back.
//
// v2 does not bake a fingerprint. It finds the UID itself, every session, by
// watching rather than assuming.
//
// WHY A FILTER IS NEEDED AT ALL
//
// The game renders all of its interface text with one pixel shader, and most
// individual strings come out as the same index count — the UID costs 156
// indices (26 quads, i.e. 26 characters). A fingerprint of {shader, index
// count} therefore also matches unrelated batches: the two reward buttons on
// the domain clear screen are drawn with the same shader and the same 156
// indices, and matching them leaves both buttons blank white. That is the bug
// this rewrite exists to close.
//
// WHAT REPLACES THE BAKED OFFSET
//
// The one property of the UID that does not drift is behavioural: it is on
// screen at all times, including while standing still in an empty field. The
// buttons are not. So every draw matching {ps, count} is recorded as a
// *candidate*, grouped by the rest of its fingerprint, and each candidate
// accumulates how many times it has been drawn. The candidate accounting for
// the largest share of the target's draws is the UID.
//
// WHY THE SHARE IS MEASURED AGAINST THE TARGET'S OWN DRAW COUNT
//
// The obvious denominator is the frame counter, and that is what the first
// build of this learner used. It was wrong, and one session's status file says
// exactly how: with 7770 presented frames, the leading candidate had been
// drawn in 1362 of them — 17%, well under the 55% threshold — and nothing was
// suppressed. But that candidate is the UID, which draws once per frame and
// therefore *cannot* be drawn in only one frame in six.
//
// 7770 / 1362 is 5.7. The add-on's present callback fires about 5.7 times per
// real frame here, so every "frame" the learner counted was really a fifth of
// one, and a 100%-of-frames batch looked like a 17%-of-frames batch. Nothing
// is wrong with the candidate; the ruler was.
//
// Counting poses of the swapchain is not a reliable measure of time, and this
// add-on has no business guessing at one. So the denominator is the number of
// draws that matched the target at all: a batch that is on screen whenever the
// game is drawing UID text has a share near 100%, and one that appears only on
// one screen has a small share. That ratio needs no notion of a frame, which is
// exactly why it is right and the previous one was not.
//
// WHY THE SHARE IS EXPONENTIAL
//
// A session-long average reads the whole session, and the player moves between
// places that draw different text. Averaged over a whole session, the batch in
// front of the player *now* is diluted by every batch that was in front of them
// before. The second session measured exactly that: the batch on screen at the
// time accounted for 100% of the recent target draws and 34.7% of the session,
// and 34.7% is under the threshold, so the UID stayed visible for the rest of
// the run even though the learner had already seen it dominate.
//
// The share is therefore an exponential moving average with a time constant of
// 2^kEmaShift target draws — about three and a half seconds of play. Old scenes
// decay out on their own, new ones take over within seconds, and no scene
// transition has to be detected: the statistics simply follow the game. Nothing
// else about the decision changed, and there is still no notion of a frame
// anywhere in it.
//
// THE SAFE FAILURE
//
// A wrong choice is the blank-white-button bug, which is worse than a visible
// UID: the user can live with a UID on screen, not with two broken buttons. So
// the add-on stands down instead of guessing:
//
//   * no decision at all until kEarlyDraws draws have matched the target, and
//     until kLearnWarmupDraws is passed every candidate must clear
//     kEarlyCoveragePct — a bar only a batch that owns essentially every target
//     draw can reach, so the opening seconds cannot be locked on a lucky streak;
//   * past kLearnWarmupDraws the bar drops to kLearnCoveragePct, which is what
//     catches a scene whose text is split across several batches;
//   * a candidate needs kLearnMinDraws draws of its own before it counts at
//     all, so a batch that flashed by briefly is never eligible;
//   * its decayed share must clear the applicable bar against the decayed
//     target total, so a batch on screen only briefly never qualifies, and a
//     batch that stops being drawn decays away within seconds;
//   * at most kMaxTargets batches are suppressed at once. More than one is
//     deliberate: a scene can split its text across several batches, and
//     insisting on a single winner there means the UID survives because no one
//     batch ever dominates. The cap is what stops that from becoming "suppress
//     everything".
//
// The statistics keep rolling for the whole session, so the choice follows the
// game: open the settlement screen and both buttons get recorded but neither
// takes over, close it and their coverage decays away.
//
// It also works through a game update without human intervention, which the
// baked fingerprint never did.
//
// WuwaUID.ini is still read once at load if it happens to exist, and its rules
// are honoured verbatim — those are exact fingerprints pinned by hand, and
// they are the escape hatch if learning ever picks the wrong batch. The file is
// never written, so deleting it changes nothing.
//
// Live status, including the full candidate table, is written to
// WuwaUID.status.txt next to the add-on.
// ---------------------------------------------------------------------------

#include <reshade.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace reshade::api;

// --------------------------------------------------------------------------- //

namespace {

// FNV-1a over a shader blob: cheap and good enough to tell PSOs apart.
uint64_t fnv1a(const void *data, size_t size, uint64_t h = 1469598103934665603ull)
{
	const auto *p = static_cast<const unsigned char *>(data);
	for (size_t i = 0; i < size; ++i) {
		h ^= static_cast<uint64_t>(p[i]);
		h *= 1099511628211ull;
	}
	return h;
}

// ---------------------------------------------------------------------------
// Draw fingerprint
// ---------------------------------------------------------------------------
struct DrawKey {
	uint64_t ps      = 0;   // pixel shader bytecode hash (0 = unknown)
	uint32_t count   = 0;   // index count, or vertex count for non-indexed draws
	uint32_t inst    = 1;   // instance count
	uint32_t first   = 0;   // first index / first vertex
	int32_t  voff    = 0;   // base vertex location
	uint32_t indexed = 1;   // 1 = DrawIndexedInstanced, 0 = DrawInstanced
	uint64_t ibo     = 0;   // index buffer offset; 0 = none bound

	bool operator==(const DrawKey &o) const
	{
		return ps == o.ps && count == o.count && inst == o.inst &&
		       first == o.first && voff == o.voff && indexed == o.indexed &&
		       ibo == o.ibo;
	}
};

// ---------------------------------------------------------------------------
// The two fields that identify the UID, and the only two that do not drift.
//
// The shader hash is of the game's compiled pixel shader, so it changes only
// with a game update that recompiles it. 156 indices is 26 quads, i.e. the 26
// characters of a UID, and is a property of the string being drawn.
//
// Everything else about the draw — above all the index buffer offset — is
// learned at runtime. See the header.
// ---------------------------------------------------------------------------
constexpr uint64_t kUidPsHash     = 0x5B44B3683F03BAE3ull;
constexpr uint32_t kUidIndexCount = 156;

// ---------------------------------------------------------------------------
// Candidate identity: everything in a fingerprint except the match condition.
//
// Two UI batches that share a shader and an index count are told apart by what
// remains. v1 assumed the index buffer offset alone was enough; keeping all the
// remaining fields costs nothing and lets the learner separate batches that
// differ only in `first` or `voff` as well.
// ---------------------------------------------------------------------------
struct CandidateKey {
	uint32_t inst    = 1;
	uint32_t first   = 0;
	int32_t  voff    = 0;
	uint32_t indexed = 1;
	uint64_t ibo     = 0;

	bool operator==(const CandidateKey &o) const
	{
		return inst == o.inst && first == o.first && voff == o.voff &&
		       indexed == o.indexed && ibo == o.ibo;
	}
};

struct CandidateKeyHash {
	size_t operator()(const CandidateKey &k) const
	{
		uint64_t h = 1469598103934665603ull;
		h = (h ^ static_cast<uint64_t>(k.inst)) * 1099511628211ull;
		h = (h ^ static_cast<uint64_t>(k.first)) * 1099511628211ull;
		h = (h ^ static_cast<uint64_t>(static_cast<int64_t>(k.voff))) * 1099511628211ull;
		h = (h ^ static_cast<uint64_t>(k.indexed)) * 1099511628211ull;
		h = (h ^ k.ibo) * 1099511628211ull;
		return static_cast<size_t>(h);
	}
};

struct Candidate {
	uint64_t draws         = 0;   // total matching draws
	uint64_t firstDraw     = 0;   // value of g_candidateDraws when first seen
	uint64_t lastDrawIndex = 0;   // ... and when last seen
	uint64_t ema           = 0;   // decayed share weight, in units of kEmaUnit
	uint64_t framesSeen    = 0;   // distinct present frames (diagnostic only)
	uint64_t lastFrameSeen = 0;   // ... and the frame that was
	uint64_t suppressed    = 0;   // draws actually blocked

	// The share is ema / g_emaTotal. Both decay by the same factor every time a
	// target draw is recorded, so the ratio between them is the decayed share
	// and needs no division by a decaying denominator kept anywhere else.
};

// ---------------------------------------------------------------------------
// Learning thresholds. Every one of these exists to make a wrong answer
// impossible rather than merely unlikely; see "THE SAFE FAILURE" in the header.
//
// All draw counts are in units of target draws, which arrive at roughly the
// rate the game redraws UI text — measured at 23 to 37 per second across two
// sessions — so kLearnWarmupDraws of 90 is about two and a half seconds of play
// and the 2^kEmaShift time constant of 64 is under two of them.
//
// Two bars, not one. Before kLearnWarmupDraws the share must clear
// kEarlyCoveragePct, which only a batch that owns essentially every target draw
// can do — exactly what a lone text batch in an instanced scene looks like.
// Making that case wait for the normal bar would burn seconds on a question the
// first two dozen draws already answered. The early bar is deliberately brutal
// because a handful of samples can spike to 85% by luck; but a spike that wrong
// decays away within one EMA constant, so the price of being fooled is under
// two seconds of one batch flickering.
// ---------------------------------------------------------------------------
constexpr uint64_t kEmaShift         = 6;     // EMA time constant = 2^kEmaShift draws
constexpr uint64_t kEmaUnit          = 1ull << kEmaShift;
constexpr uint64_t kEarlyDraws       = 24;    // earliest point a decision is allowed
constexpr uint64_t kEarlyMinDraws    = 8;     // ...and the candidate's own floor there
constexpr uint64_t kEarlyCoveragePct = 85;    // early bar: near-total ownership
constexpr uint64_t kLearnWarmupDraws = 90;    // after this the normal bar applies
constexpr uint64_t kLearnMinDraws    = 24;    // a candidate needs this many of its own
constexpr uint64_t kLearnCoveragePct = 38;    // % of the decayed target total
constexpr size_t   kMaxCandidates    = 96;
constexpr size_t   kMaxTargets       = 2;     // batches suppressed at once, at most

enum class LearnState {
	WarmingUp,    // too few target draws have passed to judge anything
	NoCandidate,  // no batch has enough decayed share yet
	Locked,       // one or more batches won, they are being suppressed
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
std::mutex g_mutex;

std::unordered_map<uint64_t, uint64_t>       g_psHashByPipeline;  // PSO handle -> PS hash
std::unordered_map<command_list *, uint64_t> g_boundPipeline;     // command list -> PSO handle
std::unordered_map<command_list *, uint64_t> g_indexBufferOffset; // command list -> index buffer offset

// Written once by load_rules() during DLL_PROCESS_ATTACH, then read-only for the
// lifetime of the process. That is what lets the draw path test it without
// taking the lock.
std::vector<DrawKey> g_blocked;

// Learned state. All of it is guarded by g_mutex.
std::unordered_map<CandidateKey, Candidate, CandidateKeyHash> g_candidates;
std::vector<CandidateKey> g_targets;      // batches currently suppressed
uint64_t     g_emaTotal   = 0;            // decayed sum of every candidate's weight
bool         g_hasLearned = false;
LearnState   g_learnState = LearnState::WarmingUp;

char g_iniPath[MAX_PATH] = {};
char g_statusPath[MAX_PATH] = {};

// Sentinels for the pixel-shader field of a fingerprint.
constexpr uint64_t kPsUnknown  = 0;   // command list has no resolved pipeline
constexpr uint64_t kPsNoShader = 1;   // pipeline resolved, but it has no pixel shader

std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_totalDraws{0};
std::atomic<uint64_t> g_suppressed{0};
std::atomic<uint64_t> g_suppressedIni{0};

// Diagnostics, surfaced in the status file. These exist so a failure can be
// localised from one run instead of guessing which event stopped firing.
std::atomic<uint64_t> g_pipelinesSeen{0};    // init_pipeline calls
std::atomic<uint64_t> g_pipelinesWithPs{0};  // ... of which carried a pixel shader
std::atomic<uint64_t> g_bindsSeen{0};        // bind_pipeline calls
std::atomic<uint64_t> g_bindsResolved{0};    // ... of which found a PS hash
std::atomic<uint64_t> g_cmdListsSeen{0};     // init_command_list calls
std::atomic<uint64_t> g_unresolvedBound{0};  // draws where no PSO was bound
std::atomic<uint64_t> g_candidateDraws{0};   // draws matching {ps, count}
std::atomic<uint64_t> g_candidatesOverflow{0}; // batches dropped at kMaxCandidates

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
CandidateKey candidate_key(const DrawKey &k)
{
	CandidateKey c;
	c.inst    = k.inst;
	c.first   = k.first;
	c.voff    = k.voff;
	c.indexed = k.indexed;
	c.ibo     = k.ibo;
	return c;
}

// Linear scan on purpose: the .ini rule list holds a handful of entries at
// most, and this runs on the draw path where an unordered_set lookup would
// cost more.
bool is_blocked_ini(const DrawKey &k)
{
	for (const DrawKey &b : g_blocked) {
		if (b == k) return true;
	}
	return false;
}

void format_key(const DrawKey &k, char *out, size_t n)
{
	snprintf(out, n,
	         "ps=%016llX count=%u inst=%u first=%u voff=%d indexed=%u ibo=%llu",
	         static_cast<unsigned long long>(k.ps), k.count, k.inst,
	         k.first, k.voff, k.indexed,
	         static_cast<unsigned long long>(k.ibo));
}

void format_candidate(const CandidateKey &c, char *out, size_t n)
{
	snprintf(out, n,
	         "inst=%u first=%u voff=%d indexed=%u ibo=%llu",
	         c.inst, c.first, c.voff, c.indexed,
	         static_cast<unsigned long long>(c.ibo));
}

const char *learn_state_name(LearnState s)
{
	switch (s) {
	case LearnState::WarmingUp:   return "warming up";
	case LearnState::NoCandidate: return "no candidate yet";
	case LearnState::Locked:      return "locked on";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// Rule loading
//
// Read-only by design. A line is
// "ps[,count[,inst[,first[,voff[,indexed[,ibo]]]]]]" in hex/dec; the trailing
// fields default to the same values the v1 built-in rule used, so a rule copied
// out of an older status file still parses.
// ---------------------------------------------------------------------------
void load_rules()
{
	const int n = GetPrivateProfileIntA("WuwaUID", "Blocked", 0, g_iniPath);
	for (int i = 0; i < n && i < 64; ++i) {
		char name[32] = {};
		char value[224] = {};
		snprintf(name, sizeof(name), "K%d", i);
		GetPrivateProfileStringA("WuwaUID", name, "", value, sizeof(value), g_iniPath);

		unsigned long long ps = 0, ibo = 0;
		unsigned count = 0, inst = 1, first = 0, indexed = 1;
		int voff = 0;
		int got = sscanf_s(value, "%llX,%u,%u,%u,%d,%u,%llu",
		                   &ps, &count, &inst, &first, &voff, &indexed, &ibo);
		if (got < 2) continue;

		DrawKey k;
		k.ps = ps;
		k.count = count;
		k.inst = inst;
		k.first = first;
		k.voff = voff;
		k.indexed = indexed;
		k.ibo = ibo;
		g_blocked.push_back(k);
	}
}

// ---------------------------------------------------------------------------
// Learning
//
// note_candidate_locked() runs on the draw path for every batch that matches
// {ps, count}; update_learned_targets_locked() runs once per frame.
//
// Both must be called with g_mutex held.
// ---------------------------------------------------------------------------
void note_candidate_locked(const DrawKey &key, uint64_t frame)
{
	const uint64_t drawIndex =
		g_candidateDraws.fetch_add(1, std::memory_order_relaxed) + 1;

	// Everyone fades a little before the new observation is added. This is what
	// makes the share follow the game instead of averaging over the whole
	// session; see "WHY THE SHARE IS EXPONENTIAL" in the header.
	for (auto &kv : g_candidates) {
		kv.second.ema -= kv.second.ema >> kEmaShift;
	}
	g_emaTotal -= g_emaTotal >> kEmaShift;

	const CandidateKey ck = candidate_key(key);

	auto it = g_candidates.find(ck);
	if (it == g_candidates.end()) {
		if (g_candidates.size() >= kMaxCandidates) {
			// Refuse to grow without bound. The UI text pool holds far fewer
			// distinct batches than this, so hitting the cap means something
			// unexpected is matching, and adding to the table would only make
			// the choice noisier.
			g_candidatesOverflow.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		it = g_candidates.emplace(ck, Candidate{}).first;
	}

	Candidate &c = it->second;
	c.draws++;
	if (c.firstDraw == 0) c.firstDraw = drawIndex;
	c.lastDrawIndex = drawIndex;
	c.ema += kEmaUnit;
	g_emaTotal += kEmaUnit;

	// The frame number is recorded for the status file only. It is not used to
	// decide anything: see "WHY THE SHARE IS MEASURED AGAINST THE TARGET'S OWN
	// DRAW COUNT" in the header for why this add-on must not trust its own
	// present counter as a measure of time.
	if (c.lastFrameSeen != frame) {
		c.framesSeen++;
		c.lastFrameSeen = frame;
	}
}

void update_learned_targets_locked()
{
	g_targets.clear();
	g_hasLearned = false;

	const uint64_t total = g_candidateDraws.load(std::memory_order_relaxed);
	if (total < kEarlyDraws) {
		g_learnState = LearnState::WarmingUp;
		return;
	}
	if (g_emaTotal == 0) {
		g_learnState = LearnState::NoCandidate;
		return;
	}

	// Which bar applies is the whole speed story. A scene that draws several
	// text batches on every frame never clears the early bar and waits for the
	// warmup; a scene dominated by a single batch is answered almost at once.
	const bool     early    = total < kLearnWarmupDraws;
	const uint64_t bar      = early ? kEarlyCoveragePct : kLearnCoveragePct;
	const uint64_t minDraws = early ? kEarlyMinDraws : kLearnMinDraws;

	// Every batch whose decayed share clears the bar, strongest first.
	// The cut is recomputed every frame from decaying numbers, so a batch that
	// stops being drawn loses its place on its own within seconds — there is no
	// recency rule to get wrong and no stale winner to unseat.
	std::vector<std::pair<uint64_t, CandidateKey>> hits;
	for (const auto &kv : g_candidates) {
		const Candidate &c = kv.second;
		if (c.draws < minDraws) continue;
		if (c.ema * 100 < g_emaTotal * bar) continue;
		hits.emplace_back(c.ema, kv.first);
	}

	if (hits.empty()) {
		g_learnState = early ? LearnState::WarmingUp : LearnState::NoCandidate;
		return;
	}

	std::sort(hits.begin(), hits.end(),
	          [](const std::pair<uint64_t, CandidateKey> &a,
	             const std::pair<uint64_t, CandidateKey> &b) {
		          return a.first > b.first;
	          });
	if (hits.size() > kMaxTargets) hits.resize(kMaxTargets);

	g_targets.reserve(hits.size());
	for (size_t i = 0; i < hits.size(); ++i) g_targets.push_back(hits[i].second);

	g_hasLearned = true;
	g_learnState = LearnState::Locked;
}

// ---------------------------------------------------------------------------
// Status file
// ---------------------------------------------------------------------------
struct CandidateRow {
	CandidateKey key;
	Candidate    stat;
};

void write_status()
{
	// Called from the present callback; snapshot under the lock, then do the
	// file I/O with the lock released.
	std::vector<DrawKey>      blocked;
	std::vector<CandidateRow> rows;
	std::vector<CandidateKey> targets;
	bool hasLearned = false;
	LearnState state = LearnState::WarmingUp;
	uint64_t targetDraws = 0;
	uint64_t emaTotal = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		blocked = g_blocked;
		rows.reserve(g_candidates.size());
		for (const auto &kv : g_candidates) {
			rows.push_back(CandidateRow{kv.first, kv.second});
		}
		targets = g_targets;
		hasLearned = g_hasLearned;
		state = g_learnState;
		targetDraws = g_candidateDraws.load(std::memory_order_relaxed);
		emaTotal = g_emaTotal;
	}

	// Sorted by decayed share, which is the quantity the decision is made on —
	// sorting by the session-long count would put a batch from a place the
	// player left an hour ago at the top of the table.
	std::sort(rows.begin(), rows.end(),
	          [](const CandidateRow &a, const CandidateRow &b) {
		          return a.stat.ema > b.stat.ema;
	          });

	FILE *f = nullptr;
	if (fopen_s(&f, g_statusPath, "w") != 0 || f == nullptr) return;

	const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
	const uint64_t frame = g_frame.load();

	fprintf(f, "WuwaUID v2 live status\n");
	fprintf(f, "======================\n");
	fprintf(f, "mode                : automatic (target learned at runtime, no hotkeys)\n");
	fprintf(f, "frames              : %llu\n", u(frame));
	fprintf(f, "draws seen          : %llu\n", u(g_totalDraws.load()));
	fprintf(f, "draws suppressed    : %llu\n", u(g_suppressed.load()));
	fprintf(f, "  of which from .ini: %llu\n", u(g_suppressedIni.load()));
	fprintf(f, "manual rules        : %zu\n", blocked.size());

	fprintf(f, "\n--- target learning ---\n");
	fprintf(f, "target              : ps=%016llX count=%u\n",
	        static_cast<unsigned long long>(kUidPsHash), kUidIndexCount);
	fprintf(f, "draws on target     : %llu\n", u(targetDraws));
	fprintf(f, "state               : %s\n", learn_state_name(state));
	if (targetDraws < kLearnWarmupDraws) {
		fprintf(f, "  early bar         : %llu%% share, from %llu draws (a lone batch)\n",
		        u(kEarlyCoveragePct), u(kEarlyDraws));
		fprintf(f, "  normal bar        : %llu%% share, after %llu draws (%llu so far)\n",
		        u(kLearnCoveragePct), u(kLearnWarmupDraws), u(targetDraws));
	}
	if (hasLearned) {
		for (size_t i = 0; i < targets.size(); ++i) {
			char k[224] = {};
			format_candidate(targets[i], k, sizeof(k));
			fprintf(f, "suppressing         : %s\n", k);
		}
	} else {
		// This is the safe failure: the UID is on screen and nothing is being
		// suppressed, which is the intended outcome when the choice is unclear.
		fprintf(f, "suppressing         : nothing\n");
	}
	if (g_candidatesOverflow.load() != 0) {
		fprintf(f, "candidate overflow  : %llu (table full at %zu)\n",
		        u(g_candidatesOverflow.load()), kMaxCandidates);
	}

	// The share column is the decision itself, so it is what the status file
	// leads with: a candidate near 100% is on screen whenever the game draws
	// UID text, and the buttons never get near that.
	fprintf(f, "\ncandidates by decayed share (time constant %llu target draws):\n",
	        u(kEmaUnit));
	if (rows.empty()) {
		fprintf(f, "  (none - the target fingerprint has not been seen)\n");
	}
	for (size_t i = 0; i < rows.size() && i < 24; ++i) {
		const CandidateRow &r = rows[i];
		char k[224] = {};
		format_candidate(r.key, k, sizeof(k));
		const uint64_t pct = (emaTotal != 0) ? (r.stat.ema * 100 / emaTotal) : 0;
		const bool leader = hasLearned &&
		                    std::find(targets.begin(), targets.end(), r.key) != targets.end();
		fprintf(f, "  [%zu] %-56s share %3llu%%  draws %llu  seen %llu..%llu%s\n",
		        i, k, u(pct), u(r.stat.draws), u(r.stat.firstDraw),
		        u(r.stat.lastDrawIndex), leader ? "   <-- suppressed" : "");
	}
	if (rows.size() > 24) {
		fprintf(f, "  ... and %zu more\n", rows.size() - 24);
	}

	fprintf(f, "\n--- diagnostics ---\n");
	fprintf(f, "pipelines seen      : %llu (with pixel shader: %llu)\n",
	        u(g_pipelinesSeen.load()), u(g_pipelinesWithPs.load()));
	fprintf(f, "pipeline binds      : %llu (PS hash resolved: %llu)\n",
	        u(g_bindsSeen.load()), u(g_bindsResolved.load()));
	fprintf(f, "command lists       : %llu\n", u(g_cmdListsSeen.load()));
	fprintf(f, "draws w/o bound PSO : %llu\n", u(g_unresolvedBound.load()));

	if (!blocked.empty()) {
		fprintf(f, "\nmanual rules (from WuwaUID.ini, always honoured):\n");
		for (size_t i = 0; i < blocked.size(); ++i) {
			char k[224] = {};
			format_key(blocked[i], k, sizeof(k));
			fprintf(f, "  [%zu] %s\n", i, k);
		}
	}

	fclose(f);
}

// ---------------------------------------------------------------------------
// ReShade event handlers
// ---------------------------------------------------------------------------
void on_init_pipeline(device *, pipeline_layout, uint32_t subobject_count,
                      const pipeline_subobject *subobjects, pipeline p)
{
	g_pipelinesSeen.fetch_add(1, std::memory_order_relaxed);

	uint64_t hash = kPsNoShader;
	for (uint32_t i = 0; i < subobject_count; ++i) {
		if (subobjects[i].type != pipeline_subobject_type::pixel_shader) continue;
		if (subobjects[i].data == nullptr || subobjects[i].count == 0) continue;

		const auto *desc = static_cast<const shader_desc *>(subobjects[i].data);
		if (desc->code != nullptr && desc->code_size > 0) {
			hash = fnv1a(desc->code, desc->code_size);
			g_pipelinesWithPs.fetch_add(1, std::memory_order_relaxed);
		}
		break;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	g_psHashByPipeline[p.handle] = hash;
}

void on_destroy_pipeline(device *, pipeline p)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_psHashByPipeline.erase(p.handle);
}

void on_init_command_list(command_list *cmd_list)
{
	g_cmdListsSeen.fetch_add(1, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline[cmd_list] = 0;
	g_indexBufferOffset[cmd_list] = 0;
}

void on_destroy_command_list(command_list *cmd_list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline.erase(cmd_list);
	g_indexBufferOffset.erase(cmd_list);
}

void on_reset_command_list(command_list *cmd_list)
{
	// A reset discards every binding made so far, including the index buffer.
	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline[cmd_list] = 0;
	g_indexBufferOffset[cmd_list] = 0;
}

void on_bind_pipeline(command_list *cmd_list, pipeline_stage, pipeline p)
{
	// Do NOT filter on `stages` here. DX12 binds a whole pipeline state object
	// in one call and ReShade does not reliably report a pixel_shader bit for
	// it, so testing the mask silently drops every graphics bind and leaves
	// every draw looking like it has no pixel shader. A compute bind is
	// harmless: it resolves to a pipeline with no pixel shader and is recorded
	// as kPsNoShader.
	g_bindsSeen.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline[cmd_list] = p.handle;

	if (g_psHashByPipeline.find(p.handle) != g_psHashByPipeline.end()) {
		g_bindsResolved.fetch_add(1, std::memory_order_relaxed);
	}
}

void on_bind_index_buffer(command_list *cmd_list, resource, uint64_t offset, uint32_t)
{
	// Only the offset is kept. The resource handle is a pointer that changes
	// every run. The offset is not stable across runs either — that is what v1
	// got wrong — but it is stable *within* a run, which is what the learner
	// needs: it only has to tell this session's UID batch apart from this
	// session's button batches.
	std::lock_guard<std::mutex> lock(g_mutex);
	g_indexBufferOffset[cmd_list] = offset;
}

// Shared tail for both draw events. Returns true to suppress the draw.
bool record_and_decide(const DrawKey &key)
{
	g_totalDraws.fetch_add(1, std::memory_order_relaxed);

	// Rules pinned by hand in WuwaUID.ini. g_blocked is written once at load
	// and never again, so reading it here needs no lock — which matters,
	// because this is the scene-geometry path.
	if (is_blocked_ini(key)) {
		g_suppressed.fetch_add(1, std::memory_order_relaxed);
		g_suppressedIni.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	// Everything that is not the UID's shader and index count is none of this
	// add-on's business, and must not pay for a lock.
	if (key.ps != kUidPsHash || key.count != kUidIndexCount)
		return false;

	// The UID and the buttons share shader and count, so this is where the
	// lock is taken on the draw path: roughly 30 draws per second.
	std::lock_guard<std::mutex> lock(g_mutex);
	note_candidate_locked(key, g_frame.load(std::memory_order_relaxed));

	if (!g_hasLearned)
		return false;

	const CandidateKey ck = candidate_key(key);
	for (size_t i = 0; i < g_targets.size(); ++i) {
		if (g_targets[i] == ck) {
			g_suppressed.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
	}
	return false;
}

DrawKey make_key(command_list *cmd_list, uint32_t count, uint32_t inst,
                 uint32_t first, int32_t voff, uint32_t indexed)
{
	DrawKey k;
	k.count = count;
	k.inst = inst;
	k.first = first;
	k.voff = voff;
	k.indexed = indexed;
	k.ps = kPsUnknown;
	k.ibo = 0;

	std::lock_guard<std::mutex> lock(g_mutex);
	const auto cl = g_boundPipeline.find(cmd_list);
	if (cl == g_boundPipeline.end() || cl->second == 0) {
		g_unresolvedBound.fetch_add(1, std::memory_order_relaxed);
		return k;
	}

	const auto ps = g_psHashByPipeline.find(cl->second);
	k.ps = (ps != g_psHashByPipeline.end()) ? ps->second : kPsNoShader;

	const auto ib = g_indexBufferOffset.find(cmd_list);
	if (ib != g_indexBufferOffset.end()) k.ibo = ib->second;

	return k;
}

bool on_draw_indexed(command_list *cmd_list, uint32_t index_count,
                     uint32_t instance_count, uint32_t first_index,
                     int32_t vertex_offset, uint32_t)
{
	const DrawKey k = make_key(cmd_list, index_count, instance_count,
	                           first_index, vertex_offset, 1);
	return record_and_decide(k);
}

bool on_draw(command_list *cmd_list, uint32_t vertex_count,
             uint32_t instance_count, uint32_t first_vertex, uint32_t)
{
	const DrawKey k = make_key(cmd_list, vertex_count, instance_count,
	                           first_vertex, 0, 0);
	return record_and_decide(k);
}

void on_present(command_queue *, swapchain *, const rect *, const rect *,
                uint32_t, const rect *)
{
	const uint64_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;

	// Re-evaluate the choice once per frame. Cheap: the candidate table holds a
	// few dozen entries at most.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		update_learned_targets_locked();
	}

	// Refresh the status file about twice per second. It is the only output
	// this add-on produces, and it is what tells you what it decided and why.
	static uint64_t lastWrite = 0;
	if (frame - lastWrite >= 30) {
		lastWrite = frame;
		write_status();
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Add-on entry points
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(instance))
			return FALSE;

		{
			char path[MAX_PATH] = {};
			GetModuleFileNameA(instance, path, MAX_PATH);
			if (char *slash = strrchr(path, '\\')) *slash = '\0';
			snprintf(g_iniPath, sizeof(g_iniPath), "%s\\WuwaUID.ini", path);
			snprintf(g_statusPath, sizeof(g_statusPath), "%s\\WuwaUID.status.txt", path);
		}

		load_rules();

		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::present>(on_present);
		break;

	case DLL_PROCESS_DETACH:
		reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::unregister_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::unregister_event<reshade::addon_event::draw>(on_draw);
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<reshade::addon_event::present>(on_present);

		reshade::unregister_addon(instance);
		break;
	}
	return TRUE;
}
