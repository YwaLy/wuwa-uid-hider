// ---------------------------------------------------------------------------
// WuwaUID — hotkey debug build
//
// THIS IS THE DIAGNOSTIC BUILD, NOT THE RECOMMENDED ONE.
//
// It exists for exactly one purpose: re-deriving the target draw when a game
// update changes its shaders and the baked-in fingerprint in the release build
// stops matching. It ships with a live candidate list and six hotkeys because
// the release build deliberately has neither.
//
// The candidate list runs into the thousands, so it is narrowed by bisection
// rather than walked one entry at a time: suppress the left half of the current
// range, look at the screen, keep whichever half still contains the UID. That
// resolves in log2(n) rounds — about 12 for a typical list.
//
//     F7   undo the previous narrowing (for when the screen was misread)
//     F8   rebuild the candidate list, reset the range to [0, all)
//     F9   suppress everything in the left half of the current range
//     F10  UID disappeared  -> keep the left half
//     F11  UID still visible -> keep the right half
//     F12  clear learned rules (built-in rules survive)
//
// When the range collapses to a single candidate it is committed automatically
// and appended to WuwaUID.ini.
//
// Everything else — pipeline indexing, per-command-list state tracking, the
// suppression itself — is the same code as the release build.
//
// Built-in rules from the release build are always active here too, so the UID
// stays hidden while you work; F12 clears only what this build learned.
// ---------------------------------------------------------------------------

#include <reshade.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

	bool operator==(const DrawKey &o) const
	{
		return ps == o.ps && count == o.count && inst == o.inst &&
		       first == o.first && voff == o.voff && indexed == o.indexed;
	}
};

struct DrawKeyHash {
	size_t operator()(const DrawKey &k) const
	{
		size_t h = static_cast<size_t>(k.ps);
		const auto mix = [&h](size_t v) {
			h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
		};
		mix(k.count);
		mix(k.inst);
		mix(k.first);
		mix(static_cast<size_t>(static_cast<int64_t>(k.voff)));
		mix(k.indexed);
		return h;
	}
};

// ---------------------------------------------------------------------------
// The target
//
// Same table as the release build. Keeping it here means the UID stays hidden
// while you hunt for a replacement, and that F12 can only clear *learned*
// rules, never these.
// ---------------------------------------------------------------------------
constexpr DrawKey kBuiltinRules[] = {
	// ps                     count  inst  first  voff  indexed
	{ 0x5B44B3683F03BAE3ull,    156,    1,     0,    0,        1 },
};

// ---------------------------------------------------------------------------
// Candidate filter
//
// A HUD string can be issued as one draw per glyph, so the per-frame cap has to
// be generous: an early value of 2.5 rejected far too much. The count cap only
// exists to keep the list a sane size.
// ---------------------------------------------------------------------------
constexpr uint64_t kMinOccurrences = 3;
constexpr uint32_t kMaxCount       = 200000;
constexpr double   kMaxPerFrame    = 32.0;
constexpr size_t   kMaxFingerprints = 200000;
constexpr size_t   kMaxUndoDepth    = 64;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
std::mutex g_mutex;

std::unordered_map<uint64_t, uint64_t>       g_psHashByPipeline;  // PSO handle -> PS hash
std::unordered_map<command_list *, uint64_t> g_boundPipeline;     // command list -> PSO handle
std::vector<DrawKey>                         g_blocked;           // suppressed fingerprints

char g_iniPath[MAX_PATH] = {};
char g_statusPath[MAX_PATH] = {};

// Sentinels for the pixel-shader field of a fingerprint.
constexpr uint64_t kPsUnknown  = 0;   // command list has no resolved pipeline
constexpr uint64_t kPsNoShader = 1;   // pipeline resolved, but it has no pixel shader

std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_totalDraws{0};
std::atomic<uint64_t> g_suppressed{0};
std::atomic<uint64_t> g_suppressedBuiltin{0};
std::atomic<uint64_t> g_suppressedProbe{0};

// Diagnostics, surfaced in the status file.
std::atomic<uint64_t> g_pipelinesSeen{0};
std::atomic<uint64_t> g_pipelinesWithPs{0};
std::atomic<uint64_t> g_bindsSeen{0};
std::atomic<uint64_t> g_bindsResolved{0};
std::atomic<uint64_t> g_cmdListsSeen{0};
std::atomic<uint64_t> g_unresolvedBound{0};

// Candidate collection (guarded by g_mutex).
struct Occurrence {
	uint64_t count      = 0;
	uint64_t firstFrame = 0;
};
std::unordered_map<DrawKey, Occurrence, DrawKeyHash> g_occurrences;

struct Candidate {
	DrawKey  key;
	uint64_t count    = 0;
	double   perFrame = 0.0;
};
std::vector<Candidate> g_candidates;

// Bisection state (guarded by g_mutex).
std::unordered_set<DrawKey, DrawKeyHash> g_probe;   // suppressed for this round
int g_probeCount = 0;
int g_lo = 0, g_hi = 0, g_mid = 0;
std::vector<std::pair<int, int>> g_history;         // undo stack of [lo, hi)
bool g_candidatesBuilt = false;

// Rejection counters, so an empty candidate list can be explained.
std::atomic<uint64_t> g_rejRare{0};
std::atomic<uint64_t> g_rejInstanced{0};
std::atomic<uint64_t> g_rejCount{0};
std::atomic<uint64_t> g_rejPerFrame{0};

std::vector<std::string> g_notes;   // last few hotkey actions, shown in status

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
bool is_builtin_rule(const DrawKey &k)
{
	for (const DrawKey &b : kBuiltinRules) {
		if (b == k) return true;
	}
	return false;
}

// Linear scan on purpose: this runs on the draw path.
bool is_blocked(const DrawKey &k)
{
	for (const DrawKey &b : g_blocked) {
		if (b == k) return true;
	}
	return false;
}

bool is_probe_hit(const DrawKey &k)
{
	return g_probeCount > 0 && g_probe.find(k) != g_probe.end();
}

void format_key(const DrawKey &k, char *out, size_t n)
{
	snprintf(out, n,
	         "ps=%016llX count=%u inst=%u first=%u voff=%d indexed=%u",
	         static_cast<unsigned long long>(k.ps), k.count, k.inst,
	         k.first, k.voff, k.indexed);
}

void add_note(const char *fmt, ...)
{
	char buf[256] = {};
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);

	if (g_notes.size() >= 6) g_notes.erase(g_notes.begin());
	g_notes.emplace_back(buf);
}

// Edge-triggered key test: true only on the frame the key goes down.
bool key_hit(int vk)
{
	static std::unordered_map<int, bool> down;
	const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
	const bool was = down[vk];
	down[vk] = now;
	return now && !was;
}

// ---------------------------------------------------------------------------
// Rule persistence
//
// The debug build is the only one that writes WuwaUID.ini — it appends what it
// learns. The release build reads the same file if it happens to be there.
// ---------------------------------------------------------------------------
void save_ini()
{
	// Callers must NOT hold g_mutex: this takes it itself.
	std::vector<DrawKey> blocked;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		blocked = g_blocked;
	}

	WritePrivateProfileStringA("WuwaUID", "Blocked", std::to_string(blocked.size()).c_str(), g_iniPath);
	for (size_t i = 0; i < blocked.size(); ++i) {
		char k[192] = {};
		format_key(blocked[i], k, sizeof(k));
		// The ini format uses commas; the formatter already produces them.
		std::string value(k);
		const size_t eq = value.find('=');
		if (eq != std::string::npos) value = value.substr(eq + 1);
		for (char &c : value) if (c == ' ') c = ',';
		char name[32] = {};
		snprintf(name, sizeof(name), "K%zu", i);
		WritePrivateProfileStringA("WuwaUID", name, value.c_str(), g_iniPath);
	}
}

// ---------------------------------------------------------------------------
// Candidate collection
// ---------------------------------------------------------------------------
void rebuild_candidates()
{
	// Callers must NOT hold g_mutex.
	std::vector<Candidate> out;
	uint64_t rejRare = 0, rejInst = 0, rejCount = 0, rejFrame = 0;
	const uint64_t frames = std::max<uint64_t>(1, g_frame.load());

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (const auto &kv : g_occurrences) {
			const DrawKey &k = kv.first;
			const Occurrence &o = kv.second;

			if (o.count < kMinOccurrences) { ++rejRare; continue; }
			if (k.inst != 1)               { ++rejInst; continue; }
			if (k.count == 0 || k.count > kMaxCount) { ++rejCount; continue; }

			const double perFrame = static_cast<double>(o.count) /
			                        static_cast<double>(std::max<uint64_t>(1, frames - o.firstFrame));
			if (perFrame > kMaxPerFrame)   { ++rejFrame; continue; }

			Candidate c;
			c.key = k;
			c.count = o.count;
			c.perFrame = perFrame;
			out.push_back(c);
		}
	}

	// A HUD element drawn exactly once per frame sorts to the front.
	std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) {
		const double da = std::fabs(a.perFrame - 1.0);
		const double db = std::fabs(b.perFrame - 1.0);
		if (da != db) return da < db;
		return a.count < b.count;
	});

	std::lock_guard<std::mutex> lock(g_mutex);
	g_candidates = std::move(out);
	g_candidatesBuilt = true;
	g_probe.clear();
	g_probeCount = 0;
	g_lo = 0;
	g_hi = static_cast<int>(g_candidates.size());
	g_mid = g_hi;
	g_history.clear();

	g_rejRare.store(rejRare, std::memory_order_relaxed);
	g_rejInstanced.store(rejInst, std::memory_order_relaxed);
	g_rejCount.store(rejCount, std::memory_order_relaxed);
	g_rejPerFrame.store(rejFrame, std::memory_order_relaxed);

	add_note("candidates rebuilt: %zu", g_candidates.size());
}

// Suppress [lo, mid) for this round.
void probe_left_half()
{
	// Callers must NOT hold g_mutex.
	std::vector<DrawKey> picked;
	int lo = 0, mid = 0, hi = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_candidatesBuilt || g_candidates.empty()) return;

		lo = g_lo;
		hi = g_hi;
		mid = lo + (hi - lo) / 2;
		g_mid = mid;
		g_probe.clear();
		for (int i = lo; i < mid && i < static_cast<int>(g_candidates.size()); ++i) {
			g_probe.insert(g_candidates[i].key);
			picked.push_back(g_candidates[i].key);
		}
		g_probeCount = static_cast<int>(g_probe.size());
		g_history.emplace_back(lo, hi);
		if (g_history.size() > kMaxUndoDepth) g_history.erase(g_history.begin());
	}
	add_note("probing [%d, %d) = %zu draws", lo, mid, picked.size());
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
void write_status()
{
	std::vector<DrawKey> blocked;
	std::vector<Candidate> candidates;
	int lo = 0, hi = 0, mid = 0, probeCount = 0;
	size_t undoDepth = 0;
	bool built = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		blocked = g_blocked;
		candidates = g_candidates;
		lo = g_lo; hi = g_hi; mid = g_mid;
		probeCount = g_probeCount;
		undoDepth = g_history.size();
		built = g_candidatesBuilt;
	}

	FILE *f = nullptr;
	if (fopen_s(&f, g_statusPath, "w") != 0 || f == nullptr) return;

	const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };

	fprintf(f, "WuwaUID live status  (HOTKEY DEBUG BUILD)\n");
	fprintf(f, "=======================================\n");
	fprintf(f, "hotkeys             : F7 undo  F8 rebuild  F9 probe left  F10 left  F11 right  F12 clear\n");
	fprintf(f, "frames              : %llu\n", u(g_frame.load()));
	fprintf(f, "draws seen          : %llu\n", u(g_totalDraws.load()));
	fprintf(f, "draws suppressed    : %llu\n", u(g_suppressed.load()));
	fprintf(f, "  of which built-in : %llu\n", u(g_suppressedBuiltin.load()));
	fprintf(f, "  of which probe    : %llu\n", u(g_suppressedProbe.load()));
	fprintf(f, "active rules        : %zu\n", blocked.size());

	fprintf(f, "\n--- bisection ---\n");
	if (!built) {
		fprintf(f, "candidates          : not built yet (press F8)\n");
	} else {
		fprintf(f, "candidates          : %zu\n", candidates.size());
		fprintf(f, "bisect range        : [%d, %d)  mid=%d\n", lo, hi, mid);
		fprintf(f, "probing now         : %d draws\n", probeCount);
		fprintf(f, "undo depth          : %zu\n", undoDepth);
		if (hi - lo == 1 && lo < static_cast<int>(candidates.size())) {
			char k[192] = {};
			format_key(candidates[lo].key, k, sizeof(k));
			fprintf(f, "converged on        : %s\n", k);
		}
		if (!candidates.empty()) {
			const int n = static_cast<int>(std::min<size_t>(candidates.size(), 5));
			fprintf(f, "\ntop candidates (best first):\n");
			for (int i = 0; i < n; ++i) {
				char k[192] = {};
				format_key(candidates[i].key, k, sizeof(k));
				fprintf(f, "  [%d] %s  per_frame=%.2f  hits=%llu\n",
				        i, k, candidates[i].perFrame,
				        static_cast<unsigned long long>(candidates[i].count));
			}
		}
	}

	fprintf(f, "\n--- filter rejects ---\n");
	fprintf(f, "rare=%llu instanced=%llu count=%llu per_frame=%llu\n",
	        u(g_rejRare.load()), u(g_rejInstanced.load()),
	        u(g_rejCount.load()), u(g_rejPerFrame.load()));
	fprintf(f, "caps: min_occurrences=%llu max_count=%u max_per_frame=%.1f\n",
	        static_cast<unsigned long long>(kMinOccurrences), kMaxCount, kMaxPerFrame);

	fprintf(f, "\n--- diagnostics ---\n");
	fprintf(f, "pipelines seen      : %llu (with pixel shader: %llu)\n",
	        u(g_pipelinesSeen.load()), u(g_pipelinesWithPs.load()));
	fprintf(f, "pipeline binds      : %llu (PS hash resolved: %llu)\n",
	        u(g_bindsSeen.load()), u(g_bindsResolved.load()));
	fprintf(f, "command lists       : %llu\n", u(g_cmdListsSeen.load()));
	fprintf(f, "draws w/o bound PSO : %llu\n", u(g_unresolvedBound.load()));

	fprintf(f, "\nblocked fingerprints:\n");
	for (size_t i = 0; i < blocked.size(); ++i) {
		char k[192] = {};
		format_key(blocked[i], k, sizeof(k));
		fprintf(f, "  [%zu] %s%s\n", i, k,
		        is_builtin_rule(blocked[i]) ? "   (built-in)" : "   (learned)");
	}

	if (!g_notes.empty()) {
		fprintf(f, "\nrecent actions:\n");
		for (const std::string &n : g_notes) fprintf(f, "  %s\n", n.c_str());
	}

	fclose(f);
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------
// Called from on_present. g_mutex is NOT held here: the helpers below take it
// themselves, and save_ini() must never run while it is held.
void handle_hotkeys()
{
	if (key_hit(VK_F8)) {
		rebuild_candidates();
	}

	if (key_hit(VK_F9)) {
		probe_left_half();
	}

	if (key_hit(VK_F10) || key_hit(VK_F11)) {
		const bool keepLeft = key_hit(VK_F10);   // note: first call already consumed it
		bool converged = false;
		std::vector<DrawKey> snapshot;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_candidatesBuilt || g_candidates.empty()) {
				// nothing to do
			} else {
				if (keepLeft) g_hi = g_mid;
				else          g_lo = g_mid;

				g_probe.clear();
				g_probeCount = 0;

				if (g_hi - g_lo == 1 && g_lo >= 0 && g_lo < static_cast<int>(g_candidates.size())) {
					const DrawKey &k = g_candidates[g_lo].key;
					if (!is_blocked(k)) {
						g_blocked.push_back(k);
						snapshot = g_blocked;
					}
					g_notes.push_back("converged - locked rule");
					converged = true;
				}
			}
		}

		if (converged) {
			save_ini();   // lock released above
			add_note("locked and written to WuwaUID.ini");
		}
	}

	if (key_hit(VK_F7)) {
		bool restored = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_history.empty()) {
				const auto prev = g_history.back();
				g_history.pop_back();
				g_lo = prev.first;
				g_hi = prev.second;
				g_mid = prev.first;
				g_probe.clear();
				g_probeCount = 0;
				restored = true;
			}
		}
		if (restored) add_note("undo: range restored");
	}

	if (key_hit(VK_F12)) {
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_blocked.clear();
			for (const DrawKey &k : kBuiltinRules) g_blocked.push_back(k);
			g_notes.clear();
		}
		add_note("learned rules cleared (built-in kept)");
		save_ini();
	}
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
}

void on_destroy_command_list(command_list *cmd_list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline.erase(cmd_list);
}

void on_reset_command_list(command_list *cmd_list)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline[cmd_list] = 0;
}

void on_bind_pipeline(command_list *cmd_list, pipeline_stage, pipeline p)
{
	// Do NOT filter on `stages`: DX12 binds a whole PSO in one call and ReShade
	// does not reliably report a pixel_shader bit for it.
	g_bindsSeen.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(g_mutex);
	g_boundPipeline[cmd_list] = p.handle;

	if (g_psHashByPipeline.find(p.handle) != g_psHashByPipeline.end()) {
		g_bindsResolved.fetch_add(1, std::memory_order_relaxed);
	}
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

	std::lock_guard<std::mutex> lock(g_mutex);
	const auto cl = g_boundPipeline.find(cmd_list);
	if (cl == g_boundPipeline.end() || cl->second == 0) {
		g_unresolvedBound.fetch_add(1, std::memory_order_relaxed);
		return k;
	}

	const auto ps = g_psHashByPipeline.find(cl->second);
	k.ps = (ps != g_psHashByPipeline.end()) ? ps->second : kPsNoShader;
	return k;
}

// Shared tail for both draw events. Returns true to suppress the draw.
bool record_and_decide(const DrawKey &key)
{
	g_totalDraws.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(g_mutex);

	// Record for the candidate list.
	if (g_occurrences.size() < kMaxFingerprints) {
		Occurrence &o = g_occurrences[key];
		if (o.count == 0) o.firstFrame = g_frame.load(std::memory_order_relaxed);
		++o.count;
	}

	if (is_blocked(key)) {
		g_suppressed.fetch_add(1, std::memory_order_relaxed);
		if (is_builtin_rule(key)) {
			g_suppressedBuiltin.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}

	if (is_probe_hit(key)) {
		g_suppressed.fetch_add(1, std::memory_order_relaxed);
		g_suppressedProbe.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	return false;
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

	handle_hotkeys();

	static uint64_t lastWrite = 0;
	if (frame - lastWrite >= 15) {   // ~4 Hz: this build is meant to be watched
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
			snprintf(g_statusPath, sizeof(g_statusPath), "%s\\WuwaUID-debug.status.txt", path);
		}

		// Built-in rules first, then whatever the ini adds, then built-ins again
		// unconditionally — the UID must be hidden before anything is pressed.
		{
			const int n = GetPrivateProfileIntA("WuwaUID", "Blocked", 0, g_iniPath);
			for (int i = 0; i < n && i < 64; ++i) {
				char name[32] = {};
				char value[192] = {};
				snprintf(name, sizeof(name), "K%d", i);
				GetPrivateProfileStringA("WuwaUID", name, "", value, sizeof(value), g_iniPath);

				unsigned long long ps = 0;
				unsigned count = 0, inst = 0, first = 0, indexed = 0;
				int voff = 0;
				if (sscanf_s(value, "%llX,%u,%u,%u,%d,%u",
				             &ps, &count, &inst, &first, &voff, &indexed) == 6) {
					DrawKey k;
					k.ps = ps; k.count = count; k.inst = inst;
					k.first = first; k.voff = voff; k.indexed = indexed;
					std::lock_guard<std::mutex> lock(g_mutex);
					if (!is_blocked(k)) g_blocked.push_back(k);
				}
			}

			std::lock_guard<std::mutex> lock(g_mutex);
			for (const DrawKey &k : kBuiltinRules) {
				if (!is_blocked(k)) g_blocked.push_back(k);
			}
		}

		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
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
		reshade::unregister_event<reshade::addon_event::draw>(on_draw);
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<reshade::addon_event::present>(on_present);

		reshade::unregister_addon(instance);
		break;
	}
	return TRUE;
}
