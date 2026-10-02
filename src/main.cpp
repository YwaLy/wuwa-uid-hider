// ---------------------------------------------------------------------------
// WuwaUID — Wuthering Waves UID hider
//
// A ReShade add-on for DirectX 12 that hides the on-screen player UID by
// suppressing exactly one draw command per frame.
//
// The target draw is baked into this binary (see kBuiltinRules) and takes
// effect the moment the add-on loads. There are no hotkeys and no runtime
// search: the add-on hides the UID and otherwise stays out of the way.
//
// It works purely through the ReShade add-on API:
//     * addon_event::init_pipeline    -> index every PSO by its pixel shader hash
//     * addon_event::bind_pipeline    -> track the PSO currently bound per command list
//     * addon_event::draw_indexed     -> return true to prevent that draw from executing
//     * addon_event::draw             -> same, for non-indexed draws
//     * addon_event::present          -> frame counter, status file
//
// No shader patching, no vtable hooking, no process injection, no DXC.
//
// WuwaUID.ini is read once at load if it happens to exist and can only *add*
// rules on top of the built-in ones. It is never written, so deleting it
// changes nothing.
//
// Live status is written to WuwaUID.status.txt next to the add-on.
// ---------------------------------------------------------------------------

#include <reshade.hpp>

#include <Windows.h>

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

	bool operator==(const DrawKey &o) const
	{
		return ps == o.ps && count == o.count && inst == o.inst &&
		       first == o.first && voff == o.voff && indexed == o.indexed;
	}
};

// ---------------------------------------------------------------------------
// The target
//
// The UID draw was isolated by bisection over the game's draw calls and is
// pinned here so hiding is active from the very first frame — no hotkey, no
// candidate list to walk, no file to keep in sync.
//
// Fields: { pixel shader bytecode hash, index count, instance count,
//           first index, base vertex, indexed (1) or not (0) }
//
// The hash is of the game's compiled pixel shader, so a game update that
// changes its shaders invalidates the rule and the UID comes back. Adding
// entries to the table below needs no other change.
// ---------------------------------------------------------------------------
constexpr DrawKey kBuiltinRules[] = {
	// ps                     count  inst  first  voff  indexed
	{ 0x5B44B3683F03BAE3ull,    156,    1,     0,    0,        1 },
};

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

// Diagnostics, surfaced in the status file. These exist so a failure can be
// localised from one run instead of guessing which event stopped firing.
std::atomic<uint64_t> g_pipelinesSeen{0};    // init_pipeline calls
std::atomic<uint64_t> g_pipelinesWithPs{0};  // ... of which carried a pixel shader
std::atomic<uint64_t> g_bindsSeen{0};        // bind_pipeline calls
std::atomic<uint64_t> g_bindsResolved{0};    // ... of which found a PS hash
std::atomic<uint64_t> g_cmdListsSeen{0};     // init_command_list calls
std::atomic<uint64_t> g_unresolvedBound{0};  // draws where no PSO was bound

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

// Linear scan on purpose: the rule list holds a handful of entries at most, and
// this runs on the draw path where an unordered_set lookup would cost more.
bool is_blocked(const DrawKey &k)
{
	for (const DrawKey &b : g_blocked) {
		if (b == k) return true;
	}
	return false;
}

void format_key(const DrawKey &k, char *out, size_t n)
{
	snprintf(out, n,
	         "ps=%016llX count=%u inst=%u first=%u voff=%d indexed=%u",
	         static_cast<unsigned long long>(k.ps), k.count, k.inst,
	         k.first, k.voff, k.indexed);
}

// ---------------------------------------------------------------------------
// Rule loading
//
// Read-only by design. The UID is baked into the binary above, so nothing here
// depends on a writable file sitting next to the add-on: editing WuwaUID.ini by
// hand *adds* rules, and deleting it changes nothing.
// ---------------------------------------------------------------------------
void load_rules()
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
			k.ps = ps;
			k.count = count;
			k.inst = inst;
			k.first = first;
			k.voff = voff;
			k.indexed = indexed;
			g_blocked.push_back(k);
		}
	}

	// The baked-in rules go in last and unconditionally: the file may be
	// missing, stale, or carry rules from an older game build, and none of
	// that may be allowed to turn the UID back on.
	for (const DrawKey &k : kBuiltinRules) {
		if (!is_blocked(k)) g_blocked.push_back(k);
	}
}

void write_status()
{
	// Called from the present callback; snapshot under the lock, then do the
	// file I/O with the lock released.
	std::vector<DrawKey> blocked;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		blocked = g_blocked;
	}

	FILE *f = nullptr;
	if (fopen_s(&f, g_statusPath, "w") != 0 || f == nullptr) return;

	const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };

	fprintf(f, "WuwaUID live status\n");
	fprintf(f, "===================\n");
	fprintf(f, "mode                : automatic (target baked in, no hotkeys)\n");
	fprintf(f, "frames              : %llu\n", u(g_frame.load()));
	fprintf(f, "draws seen          : %llu\n", u(g_totalDraws.load()));
	fprintf(f, "draws suppressed    : %llu\n", u(g_suppressed.load()));
	fprintf(f, "  of which built-in : %llu\n", u(g_suppressedBuiltin.load()));
	fprintf(f, "active rules        : %zu\n", blocked.size());
	fprintf(f, "\n--- diagnostics ---\n");
	fprintf(f, "pipelines seen      : %llu (with pixel shader: %llu)\n",
	        u(g_pipelinesSeen.load()), u(g_pipelinesWithPs.load()));
	fprintf(f, "pipeline binds      : %llu (PS hash resolved: %llu)\n",
	        u(g_bindsSeen.load()), u(g_bindsResolved.load()));
	fprintf(f, "command lists       : %llu\n", u(g_cmdListsSeen.load()));
	fprintf(f, "draws w/o bound PSO : %llu\n", u(g_unresolvedBound.load()));

	fprintf(f, "\nactive rules:\n");
	for (size_t i = 0; i < blocked.size(); ++i) {
		char k[192] = {};
		format_key(blocked[i], k, sizeof(k));
		fprintf(f, "  [%zu] %s%s\n", i, k,
		        is_builtin_rule(blocked[i]) ? "   (built-in)" : "   (from .ini)");
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

// Shared tail for both draw events. Returns true to suppress the draw.
bool record_and_decide(const DrawKey &key)
{
	g_totalDraws.fetch_add(1, std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (is_blocked(key)) {
		g_suppressed.fetch_add(1, std::memory_order_relaxed);
		if (is_builtin_rule(key)) {
			g_suppressedBuiltin.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
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

	// Refresh the status file about twice per second. It is the only output
	// this add-on produces, and it is what tells you whether the rule is
	// still matching after a game update.
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
