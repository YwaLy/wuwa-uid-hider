# WuwaUID

[![Read in English](https://img.shields.io/badge/lang-English-2ea043?style=flat-square)](README.md) [![阅读中文版](https://img.shields.io/badge/lang-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87-0969da?style=flat-square)](README.zh-CN.md)

[![Written by AI](https://img.shields.io/badge/WRITTEN%20BY-AI%20%E2%80%94%20NO%20HUMAN%20AUTHOR-ff007f?style=for-the-badge)](#-written-entirely-by-ai)
[![Anti-cheat risk](https://img.shields.io/badge/ANTI--CHEAT%20RISK-UNKNOWN-red?style=for-the-badge)](#-anti-cheat-risk)

[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![ReShade API](https://img.shields.io/badge/ReShade%20add--on%20API-20-blueviolet?style=flat-square)](#requirements)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20DirectX%2012-lightgrey?style=flat-square)](#requirements)
[![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen?style=flat-square)](#features)

A ReShade add-on that hides the on-screen player **UID** in **Wuthering Waves**
(DirectX 12) by suppressing exactly one draw call per frame.

Since **v2.0.0** the target is **learned at runtime** rather than compiled in.
The add-on watches the batches of UI text the game draws, and hides the one that
is on screen essentially all the time — which is what the UID is and what the
buttons are not. There is no fingerprint to re-measure after a game update, no
configuration file to edit, and no hotkeys. See
[How it works](#how-it-works).

![preview](docs/preview.jpg)

---

> [!CAUTION]
> ### 🔴 Written entirely by AI
>
> **Every line of this add-on — the ReShade integration, the draw-call
> fingerprinting, the learning logic, and this document — was written by an
> AI, not by a person.** No human reviewed the code for correctness or for
> security. It was verified by running it, and by the status file it writes back
> (see [Verifying it works](#verifying-it-works)); it was not verified by review.
>
> Treat it accordingly. Read the source before you run it — the whole add-on is
> a single 867-line file, and every published binary is reproducible from a
> tagged CI run.

> [!WARNING]
> ### 🟠 Anti-cheat risk — unknown
>
> Wuthering Waves ships an anti-cheat. **Whether this add-on is detectable or
> bannable is unknown.** There is no safe answer to give, and nobody has tested
> it against a ban wave.
>
> What *can* be said precisely is what the add-on does and does not do. It talks
> only to the documented ReShade add-on API: it does **not** modify game files,
> read game memory, write into the game process, inject code, or hook any
> vtable. The suppression is a supported return value from a documented
> callback.
>
> But **ReShade itself injects a DLL into the game process**, and some games
> treat that as a violation of their terms of service regardless of what the DLL
> then does. The sibling project WuwaTFR answers the same question in a single
> word: *"Unknown."*
>
> **Use at your own risk. Do not use an account you cannot afford to lose.**

## Known issues in v1.0.0

**If you are still running v1.0.0, read this.** v1 hid the UID correctly and
never lost track of it, but it had one defect, and it was reported by a user
rather than found by testing:

> [!IMPORTANT]
> ### ⚪ Blank white buttons
>
> v1 suppressed **text on buttons as well as the UID.** The button's rounded
> background was still drawn, but the label on top of it was not — so buttons
> across the UI came out as **blank white slabs** wherever the game drew text on
> them. The clearest case was the two reward buttons on the challenge-complete
> screen.
>
> Nothing else was affected: the UID stayed hidden, every other UI element
> rendered normally, and there were no crashes.

**Why it happened.** v1 matched on a fingerprint that was measured once by hand
and baked into the binary:

```
ps=5B44B3683F03BAE3  count=156  inst=1  first=0  voff=0  indexed=1
```

plus a fifth field, the index buffer offset `ibo`. Of those, only `ps`, `count`
and `ibo` ever differed between batches — the rest are constant for this game.
If the UID and the button labels were allocated from **the same UI text pool**,
then they shared an `ibo`, and the rule matched **the whole pool**: the UID was
hidden, and so was every button label in it.

**Fixed in v2.0.0.** v2 does not match a fingerprint at all. It ranks the
candidate batches by how much of the time each one is on screen, and suppresses
the ones that dominate. The UID is drawn on essentially every frame; a button
label is drawn only while its screen is open. Measured on a live session, the
UID batch accounted for 96.9% of the target's draws and the runner-up for 2.7% —
which is why the buttons are no longer touched.

> [!NOTE]
> v1.0.0 remains available at
> [its release](../../releases/tag/v1.0.0) and in the `v1.0.0` tag. It is kept
> for reference and for anyone who wants the bisection tooling that produced the
> original fingerprint; it is not recommended for use.

## Download

Every [release](../../releases) carries a single file:

| File | Size | Notes |
|---|---|---|
| **`WuwaUID.addon64`** | ~225 KB | The add-on. Drop it in and forget about it. |

The v1.0.0 release additionally carries `WuwaUID-debug-hotkeys.addon64`, a
bisection tool that is **no longer needed or shipped** — v2 needs no manual
bisection. See [Known issues in v1.0.0](#known-issues-in-v100).

v2.0.0 SHA-256:

```
2990199780AAB5BDDD41CA9C625CD07D2E3653EDFC5FFAAB50DDAF577A802910  WuwaUID.addon64
```

CI rebuilds from the tagged commit, so a fresh build may differ in bytes while
behaving identically. The source is the reference, not the hash.

## Requirements

- Wuthering Waves running in **DirectX 12** mode
- **ReShade 6.x** configured to load add-ons, with the add-on search path
  pointing at the game's `Binaries\Win64` directory
- 64-bit Windows

## Installation

Copy the file into the directory where ReShade looks for add-ons. That is
usually the same directory as the game executable:

```
<game>\Client\Binaries\Win64\
    ├── Client-Win64-Shipping.exe
    ├── dxgi.dll                 ← ReShade
    └── WuwaUID.addon64          ← this add-on
```

Or run `install.bat`, which probes the usual Steam / Epic / standalone layouts
first and only asks for a path if none matches. You can also pass the directory
explicitly:

```
install.bat "D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64"
```

ReShade loads 64-bit add-ons from files ending in `.addon64`. Check
`ReShade.log` afterwards — you should see:

```
Searching for add-ons (*.addon, *.addon64) in '...\Binaries\Win64' ...
Loading add-on from '...\WuwaUID.addon64' ...
Registered add-on "WuwaUID" v0.0.0.0 using ReShade API version 20.
```

There is **no `.ini` to place and no `.dll` to keep next to it**.

## Verifying it works

The add-on writes **`WuwaUID.status.txt`** next to itself, refreshed about
twice a second:

```
WuwaUID v2 live status
======================
mode                : automatic (target learned at runtime, no hotkeys)
frames              : 12420
draws seen          : 6417088
draws suppressed    : 657
  of which from .ini: 0
manual rules        : 0

--- target learning ---
target              : ps=5B44B3683F03BAE3 count=156
draws on target     : 5554
state               : locked on
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=14403584

candidates by decayed share (time constant 64 target draws):
  [0] inst=1 first=0 voff=0 indexed=1 ibo=14403584   share 100%  draws 3456  seen 2101..5554   <-- suppressed
  [1] inst=1 first=0 voff=0 indexed=1 ibo=24367104   share   0%  draws 1525  seen  900..2100

--- diagnostics ---
pipelines seen      : 5189 (with pixel shader: 2878)
pipeline binds      : 2881051 (PS hash resolved: 2838984)
command lists       : 72
draws w/o bound PSO : 0
```

| Reading | Meaning |
|---|---|
| `state : locked on` | A target has been chosen and is being suppressed |
| `state : warming up` | Too few target draws yet to decide — normal for the first seconds |
| `state : no candidate yet` | Nothing clears the bar right now; the UID is visible |
| `share 100%` on the suppressed row | The chosen batch is on screen essentially every frame — as a UID should be |
| `draws w/o bound PSO` is not `0` | ReShade is not reporting pipeline binds — please open an issue |
| `with pixel shader` is `0` | Wrong build loaded |

**The first seconds are the visible-cost part.** The add-on does not suppress
anything until it has seen enough draws to be sure, so the UID is on screen
briefly after launch or after a scene change:

| Situation | Time to hide |
|---|---|
| A scene dominated by one text batch (e.g. inside an instanced domain) | ~0.7 s |
| A scene with several text batches on screen at once (e.g. open world) | ~2.5 s |

## How it works

Everything happens through the documented ReShade add-on API.

1. **Index pipelines.** `addon_event::init_pipeline` fires for every new PSO.
   The pixel shader bytecode is hashed with FNV-1a into a `PSO handle → hash`
   table.

2. **Track state.** DX12 draw commands carry no state, so it has to be tracked:
   `addon_event::bind_pipeline` records which PSO each command list has bound,
   `bind_index_buffer` records the index buffer offset, and `reset_command_list`
   clears both.

   > **The one non-obvious part.** This must *not* be filtered on
   > `pipeline_stage`. DX12 binds an entire PSO in a single call and ReShade
   > does not reliably report a `pixel_shader` bit for it, so testing that mask
   > silently drops every graphics bind and leaves every draw looking like it
   > has no pixel shader. This was the bug that broke the first release.

3. **Fingerprint.** Each `draw` / `draw_indexed` yields

   ```
   DrawKey { PS hash, index count, instance count, first index, base vertex, indexed, ibo }
   ```

4. **Select the target.** Only two fields are assumed constant — the pixel
   shader hash and the index count. Everything else is learned:

   - Every batch matching those two is a **candidate**, identified by the
     remaining fields.
   - Each candidate carries an **exponentially weighted** count of how often it
     has been drawn, with a time constant of 64 target draws. A candidate's
     *share* is its weight over the total weight of all candidates.
   - A candidate whose share clears a threshold is suppressed. There are two
     thresholds, and which one applies is what sets the response time:

     | When | Bar | Why |
     |---|---|---|
     | From 24 target draws | **85% share** | Only a batch that owns essentially every draw can clear it — a lone text batch in an instanced scene. Answering that in under a second is worth a bar that a handful of samples can occasionally fake, because a wrong answer decays away within one time constant. |
     | After 90 target draws | **38% share** | Catches scenes that split their text across several batches. Measured live: the UID took 43% there and the runner-up 27%. |

   - At most **2** batches are suppressed at once. Allowing more than one is
     deliberate — in a scene that splits its text, insisting on a single winner
     means the UID survives because no one batch ever dominates. The cap is what
     stops that from becoming "suppress the entire UI".

   Nothing here knows what a frame is. Counting frames sounds natural and is
   wrong: the present callback fires a variable number of times per real frame
   (measured at 2.9× and 5.7× in two sessions), so a batch drawn on *every*
   frame once measured as 17% of "frames". Shares are therefore measured against
   the target's own draw count, which cannot drift.

5. **Suppress.** The callback returns `true`. From the ReShade documentation:

   > *To prevent this command from being executed, return `true`, otherwise
   > return `false`.*

   The draw never enters the command queue. This is a supported API behaviour,
   not a hack.

## Tuning

The thresholds in step 4 are `constexpr` at the top of
[`src/main.cpp`](src/main.cpp) and are meant to be edited:

```cpp
constexpr uint64_t kEmaShift         = 6;     // time constant = 2^6 = 64 target draws
constexpr uint64_t kEarlyDraws       = 24;    // earliest a decision is allowed
constexpr uint64_t kEarlyCoveragePct = 85;    // the early bar
constexpr uint64_t kLearnWarmupDraws = 90;    // after this the normal bar applies
constexpr uint64_t kLearnCoveragePct = 38;    // the normal bar
constexpr size_t   kMaxTargets       = 2;     // batches suppressed at once, at most
```

Lowering `kEarlyDraws` or `kEarlyCoveragePct` makes the UID vanish sooner and
makes it likelier to briefly suppress the wrong batch first. Raising
`kLearnCoveragePct` makes it more conservative. The status file is the feedback
loop: it prints the live share of every candidate.

## When it stops working

v2 has no baked-in fingerprint, so a game update that shuffles shaders or
rebuilds the UI text pool does not break it — the worst case is that it takes a
few more seconds to settle, and `ps=` in the status file changes value.

What *would* break it is a change in the structure of the problem rather than
its constants: if the game stopped drawing the UID as its own batch, or started
drawing the buttons in the same batch as the UID, then no amount of share
ranking could tell them apart. **That is the known structural weakness of this
approach** — it depends on the UID and the buttons being separable batches. If
you see the white-button symptom return, that is what happened; please open an
issue with the status file attached.

## Adding rules

Optionally, `WuwaUID.ini` next to the add-on can *add* pinned rules for batches
you want suppressed regardless of what the learner decides. It is read once at
load and is **never written** — deleting it changes nothing.

```ini
[WuwaUID]
Blocked=1
K0=5B44B3683F03BAE3,156,1,0,0,1
```

The value format is `ps,count,inst,first,voff,indexed`. Note that these are
exact fingerprints, so a rule pinned this way stops matching when the game
changes its memory layout — they are an escape hatch, not a substitute for the
learner.

## Building from source

Requires CMake ≥ 3.20 and MSVC (VS 2022 or Build Tools).

```powershell
# 1. Get the ReShade headers (any revision you intend to target)
git clone --depth 1 https://github.com/crosire/reshade.git

# 2. Configure — the include path must be ASCII, a non-ASCII path breaks header lookup
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      "-DRESHADE_INCLUDE_DIR=$PWD\reshade\include"

# 3. Build
cmake --build build --config Release

# 4. The artifact
#    build\Release\WuwaUID.addon64
```

The add-on is built against ReShade revision `aae2b7ec` (API version 20) and
tested with ReShade 6.8.0.

## Uninstalling

Delete `WuwaUID.addon64` (and optionally `WuwaUID.status.txt`). Nothing else is
touched — no registry keys, no files elsewhere, no traces in the game directory.

## How the target was found

This section documents how v1's fingerprint was obtained. **v2 does not need
any of it** — it is kept because the method is the interesting part, and because
the `v1.0.0` tag still ships the tool.

The target was narrowed down by **bisecting the game's draw calls**. The
candidate list runs into the thousands, so walking it one entry at a time is not
practical; halving the range resolves it in `log2(n)` steps.

| Key | Action |
|---|---|
| `F7` | Undo the previous narrowing (for when the screen was misread) |
| `F8` | Rebuild the candidate list, reset the range to `[0, all)` |
| `F9` | Suppress everything in the left half of the current range |
| `F10` | UID disappeared → keep the left half |
| `F11` | UID still visible → keep the right half |
| `F12` | Clear learned rules (built-in rules survive) |

When the range collapses to a single candidate it is committed automatically. It
converged in `log2(2115) ≈ 12` rounds, and the recorded undo depth was exactly
`12` — every round was answered correctly.

**Two mistakes worth recording:**

1. **The first attempt converged on the wrong target** — `count=3`, a single
   triangle, which cannot be a UID. One of the 12 rounds had been misread, and
   there was no undo at the time. That is why `F7` exists.

2. **The per-frame cap was too tight.** The initial threshold of 2.5 rejected
   far too much; a multi-glyph HUD string can be issued as **one draw per
   character**, so a reasonable cap is much higher (32 was used).

**Candidate filter**, for anyone redoing this: seen ≥ 3 times, `inst == 1`,
`count ∈ (0, 200000]`, ≤ 32 draws per frame, ranked by `|per_frame − 1|`
ascending — a HUD element drawn exactly once per frame naturally sorts to the
front.

The fingerprint it produced, which is what v1 baked in:

```
ps=5B44B3683F03BAE3  count=156  inst=1  first=0  voff=0  indexed=1
```

156 indices is 52 triangles, which matches the geometry of a short string of
digits plus its background.

**What the bisection got wrong** is recorded in
[Known issues in v1.0.0](#known-issues-in-v100): the measurement was correct,
but the fingerprint described a *pool* of batches rather than the UID alone, and
nothing in the process would have revealed that. Finding the target was never
the hard part — telling it apart from the things drawn the same way was.

## Credits

- [ReShade](https://reshade.me/) by crosire — the add-on API this is built on
- [WuwaTFR](https://github.com/wuwatfr/wuthering-waves-transparency-filter-remover)
  — a sibling add-on for the same game; its source was read while working out
  the ReShade DX12 event model, and the install layout here follows it

## License

[MIT](LICENSE). A Chinese translation of this document is available at
[README.zh-CN.md](README.zh-CN.md).
