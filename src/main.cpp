// ---------------------------------------------------------------------------
// WuwaUID v4 — Wuthering Waves UID hider, self-calibrating
//
// A ReShade add-on for DirectX 12 that hides the on-screen player UID by
// suppressing the draw commands that render it.
//
// WHAT v3 DID AND WHY IT STILL LEAKED THE UID
//
// v3 learned the UID batch by measuring the DUTY CYCLE of every batch that
// matched {pixel shader, index count}:
//
//     duty = draws / (lastFrame - firstFrame + 1)
//
// and suppressed every batch that reached 85% duty over at least 120 frames
// with at least 300 draws. The threshold was calibrated on one long session
// that happened to report 98% / 91% / 87% for its resident batches. A later
// session says the ruler does not travel:
//
//     frames 15390, draws seen 5316909, draws on target 5445,
//     draws suppressed 0, state "no candidate yet"
//
//     [0] ibo=5646848   duty  58%  draws 2505  span 11094..15386
//     [1] ibo=14920192 duty  23%  draws  960  span  4788..8948
//     [2] ibo=26119936 duty  30%  draws  872  span  1680..4538
//     [3] ibo=22969856 duty 100%  draws  232  span 14360..14591
//
// Three separate faults are visible in that table, and each one alone is
// enough to leave the UID on screen:
//
//   1. THE DENOMINATOR IS FRAMES, AND UI TEXT IS NOT REDRAWN EVERY FRAME.
//      5445 target draws across 15390 presented frames is 0.35 draws/frame:
//      the game redraws its interface text roughly 3 times per second here,
//      not 84. A batch that is on screen every single time the interface is
//      drawn still only reaches 35% of frames. v3's 85% bar was measuring the
//      game's redraw cadence, not the batch's persistence. The one batch that
//      did reach 100% ([3]) is a burst of 232 draws in 232 frames — an
//      interface element that existed for four seconds, which is exactly what
//      the bar was supposed to reject.
//
//   2. ONE LOGICAL ELEMENT ARRIVES AS SEVERAL BATCHES. The UID's slice of the
//      shared UI index pool is re-cut as the game loads scenes, so one session
//      produces a chain of incarnations. In the table above the three long
//      spans tile the session end to end — 1680..4538, 4788..8948,
//      11094..15386 — which is one element drawn from three different buffers,
//      not three elements. v3 judged each incarnation alone, so every one of
//      them was diluted by the gaps the others occupied.
//
//   3. LOCKING WAS RECOMPUTED, NOT REMEMBERED. v3 rebuilt its suppression set
//      from scratch every frame. A batch that qualified and then kept its span
//      growing without being drawn watched its own duty fall, dropped back
//      under the bar, and let the UID return — which is the flicker the user
//      reports as "the UID is back sometimes".
//
// WHAT v4 CHANGES
//
//   * The bar is refitted to the measurements above instead of to the one
//     session that flattered v3: 20% duty rather than 85%. The three long-lived
//     batches clear it; every short burst in the table is already excluded by
//     the 300-draw minimum. This is what "the batch is drawn a substantial
//     fraction of the time it exists" looks like once the redraw cadence is
//     taken out of the measurement.
//
//   * There are two ways to qualify, because the two kinds of evidence answer
//     different questions:
//
//       ENDURANCE - twelve hundred frames (fourteen seconds), and on a long
//       session a twelfth of it as well, capped at twenty-four hundred so the
//       bar can never outrun the incarnations it exists to accept. This is the
//       slow proof that a batch is not a screen somebody is reading.
//
//       EXCLUSIVITY - six hundred frames, during which the batch was alone in
//       its frame at least three hundred times, and on at least a quarter of
//       the frames it drew. The UID is the only fingerprinted element the game
//       draws for most of a session, so this is the quick proof, and it is why
//       a fresh incarnation now locks in seven seconds rather than eighteen.
//
//     A settlement screen can take neither route: the HUD is still behind it,
//     so its frames are never exclusive, and it does not survive long enough to
//     earn endurance.
//
//   * Suppression is REMEMBERED. Once a batch is confirmed it stays confirmed
//     for the rest of the session (with a seven-minute idle eviction), so the
//     UID cannot come back because its own statistics drifted.
//
//   * Incarnations are INHERITED. When the confirmed batch stops being drawn
//     and a new batch appears alone in a frame — the signature of the UID
//     moving to a fresh slice of the index pool — the new batch is adopted into
//     the confirmed set and suppressed from that frame on.
//
// The safe failure is unchanged: a wrong choice blanks a button, which is worse
// than a visible UID, so adoption is deliberately narrow. It happens only in
// frames where the candidate is the ONLY batch matching the fingerprint, only
// for a batch that has already been drawn sixty times, and only while some
// confirmed incarnation was itself drawn within the last twenty-four hundred
// frames.
//
// Live status, including the full candidate table, is written to
// WuwaUID.status.txt next to the add-on. Pressing F9 dumps the last 900
// frames of per-frame UID-fingerprint draws to WuwaUID.snapshot.txt.
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
	uint64_t draws      = 0;   // total matching draws
	uint64_t firstFrame = 0;   // first present-frame this batch was drawn on
	uint64_t lastFrame  = 0;   // ... and the most recent one
	uint64_t firstDraw  = 0;   // value of g_candidateDraws when first seen
	uint64_t lastDraw   = 0;   // ... and when last seen
	uint64_t suppressed = 0;   // draws actually blocked
	uint64_t maxGap     = 0;   // longest run of frames it was absent for
	uint64_t segments   = 0;   // how many separate stretches it was drawn in
	uint64_t soloFrames = 0;   // frames where it was the ONLY match in flight
};

// A batch that has been confirmed as (an incarnation of) the UID.
struct LockedEntry {
	uint64_t lockedAtFrame  = 0;
	uint64_t lastActiveFrame = 0;
	uint32_t inherited      = 0;   // 0 = met the bar itself, 1 = adopted
};

// ---------------------------------------------------------------------------
// v4 thresholds.
//
// All of these are fitted to the status table quoted in the header, not to the
// session v3 was tuned on:
//
//   kMinHits 300        separates the three long-lived batches (2505/960/872)
//                       from every interface burst (232 and below).
//   kMinDutyPct 20      the redraw cadence means a resident batch sits at
//                       23-58% of frames, not 85-100%.
//   kMinSpanFrames 1200 fourteen seconds. This is the only thing standing
//                       between a resident batch and a settlement screen left
//                       open: that screen draws every frame, so its hit count
//                       and its duty both look excellent. What it cannot fake
//                       is age, and a player who stops to read the screen
//                       rarely stops for fourteen seconds.
//   kMinSpanSharePct 12 ... and on a long session age alone is not enough
//                       either, since fourteen seconds of a six-minute session
//                       is still a small part of it. The bar therefore tracks
//                       the session, capped by kMaxRequiredSpan so that it can
//                       never outrun the incarnations it exists to accept.
//   kStaleFrames 36000  seven minutes without a draw and an entry is retired.
// ---------------------------------------------------------------------------
constexpr uint64_t kMinHits        = 300;
constexpr uint64_t kMinSpanFrames  = 1200;
constexpr uint64_t kMinSpanSharePct = 12;
constexpr uint64_t kMaxRequiredSpan = 2400;  // ... but never demand more than this
constexpr uint64_t kMinDutyPct     = 20;
constexpr uint64_t kFastSpanFrames = 600;   // seven seconds, when exclusivity proves it
constexpr uint64_t kMinSoloFrames  = 300;   // frames this batch had the screen to itself
constexpr uint64_t kMinSoloSharePct = 25;   // ... as a share of the draws it made
constexpr uint64_t kMinAdoptDraws  = 60;    // evidence a batch needs to be adopted
constexpr uint64_t kHandoverGap    = 2400;  // how long an incarnation stays adoptable
constexpr uint64_t kStaleFrames    = 36000; // idle eviction for a locked entry
constexpr size_t   kMaxCandidates  = 512;
constexpr size_t   kMaxTargets     = 8;     // safety cap, not a tuning knob
constexpr size_t   kFrameLogMax    = 900;   // per-frame log for the F9 snapshot

enum class LearnState {
	WarmingUp,    // too few target draws have passed to judge anything
	NoCandidate,  // no batch has cleared the bar yet
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
std::unordered_map<CandidateKey, LockedEntry, CandidateKeyHash> g_locked;
bool         g_hasLearned = false;
LearnState   g_learnState = LearnState::WarmingUp;

// Batches matching the fingerprint in the frame currently being recorded.
std::vector<CandidateKey> g_frameKeys;
uint64_t                  g_frameKeysStamp = 0;

// Per-frame log, kept as a ring buffer for the F9 snapshot.
struct FrameRecord {
	uint64_t                  frame = 0;
	std::vector<CandidateKey> keys;
};
std::vector<FrameRecord> g_frameLog;
size_t                   g_frameLogHead = 0;
bool                     g_frameLogFull = false;

char g_iniPath[MAX_PATH] = {};
char g_statusPath[MAX_PATH] = {};
char g_snapshotPath[MAX_PATH] = {};

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
std::atomic<uint64_t> g_framesNoTarget{0};   // frames with no match at all
std::atomic<uint64_t> g_framesSolo{0};       // frames with exactly one match
std::atomic<uint64_t> g_framesMulti{0};      // frames with two or more matches
std::atomic<uint64_t> g_adoptions{0};        // incarnations adopted by handover
std::atomic<uint64_t> g_snapshotWanted{0};   // F9 seen, write on the next present

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

// The span a batch must reach on this session before it may be confirmed.
//
// The share term is what keeps a settlement screen left open from looking
// resident: its buttons live for as long as the screen does, which is long in
// absolute frames but a small part of a session. A pure share, though, would
// demand a forty-minute-old batch after an hour of play and reject every
// incarnation the UID actually uses, so the bar is capped: 300 frames early on,
// rising to 2400 frames (forty seconds) as the session grows, and no further.
uint64_t required_span(uint64_t frame)
{
	const uint64_t share = frame * kMinSpanSharePct / 100;
	uint64_t need = (share > kMinSpanFrames) ? share : kMinSpanFrames;
	if (need > kMaxRequiredSpan) need = kMaxRequiredSpan;
	return need;
}

// ---------------------------------------------------------------------------
// Rule loading (read-only; WuwaUID.ini is never written)
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
// {ps, count}. settle_frame_locked() and update_locks_locked() run once per
// frame from the present callback.
//
// All three must be called with g_mutex held.
// ---------------------------------------------------------------------------
void note_candidate_locked(const DrawKey &key, uint64_t frame)
{
	const uint64_t drawIndex =
		g_candidateDraws.fetch_add(1, std::memory_order_relaxed) + 1;

	const CandidateKey ck = candidate_key(key);

	auto it = g_candidates.find(ck);
	if (it == g_candidates.end()) {
		if (g_candidates.size() >= kMaxCandidates) {
			// Table full. Evict the batch that has gone longest without being
			// drawn: one that flashed once early and never came back is exactly
			// what should lose its slot to the UID's next incarnation.
			auto oldest = g_candidates.begin();
			for (auto i = g_candidates.begin(); i != g_candidates.end(); ++i)
				if (i->second.lastFrame < oldest->second.lastFrame) oldest = i;
			g_candidates.erase(oldest);
			g_candidatesOverflow.fetch_add(1, std::memory_order_relaxed);
		}
		it = g_candidates.emplace(ck, Candidate{}).first;
	}

	Candidate &c = it->second;
	if (c.firstFrame != 0 && frame > c.lastFrame + 1) {
		const uint64_t gap = frame - c.lastFrame - 1;
		if (gap > c.maxGap) c.maxGap = gap;
		c.segments++;
	}
	c.draws++;
	if (c.firstFrame == 0) c.firstFrame = frame;
	c.lastFrame = frame;
	if (c.firstDraw == 0) c.firstDraw = drawIndex;
	c.lastDraw = drawIndex;

	// Record this batch as present in the frame currently being built.
	if (g_frameKeysStamp != frame) {
		g_frameKeys.clear();
		g_frameKeysStamp = frame;
	}
	bool seen = false;
	for (const CandidateKey &k : g_frameKeys) {
		if (k == ck) { seen = true; break; }
	}
	if (!seen) g_frameKeys.push_back(ck);
}

// Close out the frame that was just recorded: update the solo/multi histogram,
// log it for the snapshot, and adopt an incarnation if this is the moment.
void settle_frame_locked(uint64_t frame)
{
	// Draws are recorded while the frame is being built, which happens before
	// this callback fires, so anything collected belongs to the frame just gone:
	// stamp == frame - 1. An older stamp means this frame had no match at all.
	// Without this test the previous frame's set would be counted again on every
	// empty frame, inflating the solo histogram and re-running the adoption rule
	// on stale evidence.
	if (g_frameKeys.empty() || g_frameKeysStamp + 1 != frame) {
		g_framesNoTarget.fetch_add(1, std::memory_order_relaxed);
		g_frameKeys.clear();
		return;
	}

	// Snapshot ring buffer.
	if (g_frameLog.size() < kFrameLogMax) {
		g_frameLog.push_back(FrameRecord{g_frameKeysStamp, g_frameKeys});
	} else {
		if (!g_frameLogFull) g_frameLogFull = true;
		g_frameLog[g_frameLogHead] = FrameRecord{g_frameKeysStamp, g_frameKeys};
	}
	g_frameLogHead = (g_frameLogHead + 1) % kFrameLogMax;

	if (g_frameKeys.size() != 1) {
		g_framesMulti.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	g_framesSolo.fetch_add(1, std::memory_order_relaxed);

	const CandidateKey k = g_frameKeys[0];
	const uint64_t at = g_frameKeysStamp;

	auto cit = g_candidates.find(k);
	if (cit != g_candidates.end()) cit->second.soloFrames++;

	if (g_locked.find(k) != g_locked.end()) return;

	// The UID moved to a fresh slice of the index pool: the confirmed batch has
	// just stopped and this one is alone in the frame. Adopt it - but only once
	// it has shown that it keeps coming back. A batch that happened to be alone
	// on a single frame, in the gap between two incarnations, is not the UID; it
	// is whatever else the game drew while the UID was away.
	if (cit == g_candidates.end() || cit->second.draws < kMinAdoptDraws) return;

	uint64_t mostRecent = 0;
	for (const auto &e : g_locked) {
		if (e.second.lastActiveFrame > mostRecent) mostRecent = e.second.lastActiveFrame;
	}
	if (mostRecent == 0 || at <= mostRecent) return;
	if (at - mostRecent > kHandoverGap) return;
	if (g_locked.size() >= kMaxTargets) return;

	LockedEntry le;
	le.lockedAtFrame   = at;
	le.lastActiveFrame = at;
	le.inherited       = 1;
	g_locked.emplace(k, le);
	g_adoptions.fetch_add(1, std::memory_order_relaxed);

	(void)frame;
}

// Confirm batches that meet the bar, and retire entries that stopped being
// drawn long ago. The set only ever grows within a session; that is the fix for
// v3's flicker.
//
// There are two ways in, because the two kinds of evidence answer different
// questions. Endurance is the slow proof: a batch drawn for fourteen seconds
// that still comes back a fifth of the time is not a screen somebody is
// reading. Exclusivity is the quick proof: the UID is the only fingerprinted
// element the game draws for most of a session, so a batch that keeps being
// drawn on frames where nothing else matched is resident and can be confirmed
// in seven seconds. A settlement screen never earns that proof - the HUD is
// still behind it, so its frames are never exclusive - which leaves it only the
// slow route, and slow is precisely what the endurance bar refuses.
void update_locks_locked(uint64_t frame)
{
	const uint64_t needSpan = required_span(frame);

	for (const auto &kv : g_candidates) {
		const CandidateKey &k = kv.first;
		const Candidate   &c = kv.second;
		if (g_locked.find(k) != g_locked.end()) continue;
		if (c.draws < kMinHits) continue;
		if (c.firstFrame == 0) continue;

		const uint64_t span = c.lastFrame - c.firstFrame + 1;
		if (c.draws * 100 < span * kMinDutyPct) continue;

		const bool enduring  = span >= needSpan;
		const bool exclusive = span >= kFastSpanFrames &&
		                       c.soloFrames >= kMinSoloFrames &&
		                       c.soloFrames * 100 >= c.draws * kMinSoloSharePct;
		if (!enduring && !exclusive) continue;

		if (g_locked.size() >= kMaxTargets) {
			auto oldest = g_locked.begin();
			for (auto i = g_locked.begin(); i != g_locked.end(); ++i)
				if (i->second.lastActiveFrame < oldest->second.lastActiveFrame) oldest = i;
			g_locked.erase(oldest);
		}

		LockedEntry le;
		le.lockedAtFrame   = frame;
		le.lastActiveFrame = c.lastFrame;
		le.inherited       = 0;
		g_locked.emplace(k, le);
	}

	// Retire entries that have not been drawn for seven minutes. The UID comes
	// back after a loading screen, but never after that long.
	for (auto it = g_locked.begin(); it != g_locked.end(); ) {
		if (frame > it->second.lastActiveFrame &&
		    frame - it->second.lastActiveFrame > kStaleFrames)
			it = g_locked.erase(it);
		else
			++it;
	}

	if (g_locked.empty()) {
		g_learnState = (g_candidateDraws.load(std::memory_order_relaxed) < kMinHits)
		               ? LearnState::WarmingUp : LearnState::NoCandidate;
		g_hasLearned = false;
		return;
	}

	g_hasLearned = true;
	g_learnState = LearnState::Locked;
}

// ---------------------------------------------------------------------------
// Status file
// ---------------------------------------------------------------------------
bool g_locked_contains(const CandidateKey &k)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_locked.find(k) != g_locked.end();
}

struct CandidateRow {
	CandidateKey key;
	Candidate    stat;
};

void write_status()
{
	std::vector<DrawKey>      blocked;
	std::vector<CandidateRow> rows;
	std::vector<std::pair<CandidateKey, LockedEntry>> locks;
	bool hasLearned = false;
	LearnState state = LearnState::WarmingUp;
	uint64_t targetDraws = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		blocked = g_blocked;
		rows.reserve(g_candidates.size());
		for (const auto &kv : g_candidates) {
			rows.push_back(CandidateRow{kv.first, kv.second});
		}
		for (const auto &kv : g_locked) locks.push_back(kv);
		hasLearned = g_hasLearned;
		state = g_learnState;
		targetDraws = g_candidateDraws.load(std::memory_order_relaxed);
	}

	std::sort(rows.begin(), rows.end(),
	          [](const CandidateRow &a, const CandidateRow &b) {
		          return a.stat.draws > b.stat.draws;
	          });
	std::sort(locks.begin(), locks.end(),
	          [](const std::pair<CandidateKey, LockedEntry> &a,
	             const std::pair<CandidateKey, LockedEntry> &b) {
		          return a.second.lockedAtFrame < b.second.lockedAtFrame;
	          });

	FILE *f = nullptr;
	if (fopen_s(&f, g_statusPath, "w") != 0 || f == nullptr) return;

	const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
	const uint64_t frame = g_frame.load();

	fprintf(f, "WuwaUID v4 live status\n");
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
	fprintf(f, "bar                 : %llu draws, %llu%% duty, span >= %llu frames (now)\n",
	        u(kMinHits), u(kMinDutyPct), u(required_span(frame)));
	fprintf(f, "incarnations adopted: %llu\n", u(g_adoptions.load()));

	if (locks.empty()) {
		fprintf(f, "suppressing         : nothing\n");
	} else {
		for (const auto &e : locks) {
			char k[224] = {};
			format_candidate(e.first, k, sizeof(k));
			fprintf(f, "suppressing         : %s%s (locked@%llu last@%llu)\n",
			        k, e.second.inherited ? " [inherited]" : "",
			        u(e.second.lockedAtFrame), u(e.second.lastActiveFrame));
		}
	}
	if (g_candidatesOverflow.load() != 0) {
		fprintf(f, "candidate overflow  : %llu (table full at %zu)\n",
		        u(g_candidatesOverflow.load()), kMaxCandidates);
	}

	// The duty column is the decision itself. `solo` counts the frames in which
	// this batch was the only match in flight — that is the population the
	// adoption rule draws from, and a UID batch should dominate it.
	fprintf(f, "\ncandidates (bar: %llu draws, %llu%% duty, span >= %llu):\n",
	        u(kMinHits), u(kMinDutyPct), u(required_span(frame)));
	if (rows.empty()) {
		fprintf(f, "  (none - the target fingerprint has not been seen)\n");
	}
	for (size_t i = 0; i < rows.size() && i < 24; ++i) {
		const CandidateRow &r = rows[i];
		char k[224] = {};
		format_candidate(r.key, k, sizeof(k));
		const uint64_t span = (r.stat.lastFrame >= r.stat.firstFrame)
		                      ? (r.stat.lastFrame - r.stat.firstFrame + 1) : 0;
		const uint64_t duty = (span != 0) ? (r.stat.draws * 100 / span) : 0;
		const bool leader = g_locked_contains(r.key);
		fprintf(f, "  [%zu] %-56s duty %3llu%%  draws %6llu  span %llu..%llu  solo %llu  gap %llu  seg %llu%s\n",
		        i, k, u(duty), u(r.stat.draws), u(r.stat.firstFrame),
		        u(r.stat.lastFrame), u(r.stat.soloFrames), u(r.stat.maxGap),
		        u(r.stat.segments), leader ? "   <-- suppressed" : "");
	}
	if (rows.size() > 24) {
		fprintf(f, "  ... and %zu more\n", rows.size() - 24);
	}

	fprintf(f, "\n--- per-frame match count ---\n");
	fprintf(f, "  frames with 0 matches : %llu\n", u(g_framesNoTarget.load()));
	fprintf(f, "  frames with 1 match   : %llu\n", u(g_framesSolo.load()));
	fprintf(f, "  frames with 2+ matches: %llu\n", u(g_framesMulti.load()));

	fprintf(f, "\n--- diagnostics ---\n");
	fprintf(f, "pipelines seen      : %llu (with pixel shader: %llu)\n",
	        u(g_pipelinesSeen.load()), u(g_pipelinesWithPs.load()));
	fprintf(f, "pipeline binds      : %llu (PS hash resolved: %llu)\n",
	        u(g_bindsSeen.load()), u(g_bindsResolved.load()));
	fprintf(f, "command lists       : %llu\n", u(g_cmdListsSeen.load()));
	fprintf(f, "draws w/o bound PSO : %llu\n", u(g_unresolvedBound.load()));
	fprintf(f, "snapshot            : press F9 to dump the last %zu frames\n", kFrameLogMax);

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

// F9 snapshot: the last kFrameLogMax frames, oldest first, one frame per line.
void write_snapshot()
{
	std::vector<FrameRecord> log;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		log.reserve(g_frameLog.size());
		if (g_frameLogFull) {
			for (size_t i = 0; i < g_frameLog.size(); ++i)
				log.push_back(g_frameLog[(g_frameLogHead + i) % g_frameLog.size()]);
		} else {
			log = g_frameLog;
		}
	}

	FILE *f = nullptr;
	if (fopen_s(&f, g_snapshotPath, "w") != 0 || f == nullptr) return;

	const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
	fprintf(f, "WuwaUID v4 snapshot - per-frame draws matching ps=%016llX count=%u\n",
	        static_cast<unsigned long long>(kUidPsHash), kUidIndexCount);
	fprintf(f, "frame, match_count, then one 'inst/first/voff/indexed/ibo' set per match\n\n");
	for (const FrameRecord &r : log) {
		fprintf(f, "%llu %zu", u(r.frame), r.keys.size());
		for (const CandidateKey &k : r.keys) {
			fprintf(f, "  %u/%u/%d/%u/%llu", k.inst, k.first, k.voff, k.indexed,
			        static_cast<unsigned long long>(k.ibo));
		}
		fprintf(f, "\n");
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
	// every draw looking like it has no pixel shader.
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
	// every run, and the offset is not stable across runs either — but within a
	// run it is what tells this session's incarnations apart.
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

	std::lock_guard<std::mutex> lock(g_mutex);
	const uint64_t frame = g_frame.load(std::memory_order_relaxed);
	note_candidate_locked(key, frame);

	const CandidateKey ck = candidate_key(key);
	auto it = g_locked.find(ck);
	if (it == g_locked.end())
		return false;

	it->second.lastActiveFrame = frame;
	if (auto cit = g_candidates.find(ck); cit != g_candidates.end())
		cit->second.suppressed++;
	g_suppressed.fetch_add(1, std::memory_order_relaxed);
	return true;
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

	// Close out the frame just recorded, then re-evaluate the confirmed set.
	// Both are cheap: the candidate table holds a few dozen entries at most.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		settle_frame_locked(frame);
		update_locks_locked(frame);
	}

	// F9 dumps the recent per-frame log so a leak can be traced afterwards.
	static bool f9Down = false;
	const bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
	if (f9Now && !f9Down) g_snapshotWanted.store(1, std::memory_order_relaxed);
	f9Down = f9Now;
	if (g_snapshotWanted.exchange(0, std::memory_order_relaxed) != 0)
		write_snapshot();

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
			snprintf(g_snapshotPath, sizeof(g_snapshotPath), "%s\\WuwaUID.snapshot.txt", path);
		}

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_frameLog.reserve(kFrameLogMax);
			g_frameKeys.reserve(8);
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
