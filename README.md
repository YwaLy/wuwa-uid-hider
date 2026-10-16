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
The add-on watches the batches of UI text the game draws and suppresses the ones
that are either on screen essentially all the time, or drawn at times when
nothing else is — which is what the UID is and what a button label is not. There
is no fingerprint to re-measure after a game update, no configuration file to
edit, and no hotkeys.

**v2.1.0 replaces v2.0.0's criterion with two independent ones.** v2.0.0 measured
a single quantity — a batch's share of the target's draws — and that measurement
failed in both directions: it still blanked the labels on long-lived menus, and
when it was tightened the UID stopped being hidden at all. A single ratio cannot
separate "on screen constantly" from "on screen for a while", so v2.1.0 asks two
different questions instead of one, and a batch has to answer one of them
convincingly. See [How it works](#how-it-works).

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
> a single 1003-line file, and every published binary is reproducible from a
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

**Partly fixed in v2.0.0, completely fixed in v2.1.0.** v2 does not match a
fingerprint at all. It ranks the candidate batches by how much of the time each
one is on screen and suppresses the ones that dominate. The UID is drawn far more
often than any button label is; measured on a live session it took 96.9% of the
target's draws against the runner-up's 2.7%.

That fixed the *ranking*. It did not fix the *window*, and the next section is
about what the window broke.

> [!NOTE]
> v1.0.0 remains available at
> [its release](../../releases/tag/v1.0.0) and in the `v1.0.0` tag. It is kept
> for reference and for anyone who wants the bisection tooling that produced the
> original fingerprint; it is not recommended for use.

## Known issues in v2.0.0

**If you are running v2.0.0, the UID is not reliably hidden.** There are two
defects, pulling in opposite directions, and the second one was found while
fixing the first.

> [!IMPORTANT]
> ### ⚪ Blank buttons again — but only in long-lived menus
>
> The white-button symptom from v1 came back, in a narrower form: labels went
> blank once a menu had been open for a few seconds. The challenge-complete
> screen with its two reward buttons was the reproduction.

**Why.** A share is measured over a **moving window**. v2.0.0's window was an
exponentially weighted count with a time constant of 64 target draws — roughly
**3.5 seconds**. A batch drawn on every frame of that window reaches a 100%
share regardless of how rarely it is drawn in absolute terms, so the bar said
nothing about how long the batch had been there. A menu left open for more than
about three seconds pulled its labels past the 38% bar and had them suppressed
along with the UID.

The lesson: **a ratio has no memory of how long it has been held.** "What
fraction of recent draws is this batch?" cannot separate a batch drawn every
frame forever from one drawn every frame for the last four seconds.

> [!IMPORTANT]
> ### ⚪ The UID is visible again
>
> Tightening the share bar to compensate produced v2.1.0's real predecessor — an
> internal revision that was never released. Its criterion was a **duty cycle**:
> of the frames between a batch's first and last draw, what fraction contained a
> draw. The bar was 85%, with a minimum of 300 draws. On a live session that bar
> was never cleared. After fifteen thousand frames the status file read
> `draws suppressed: 0`, and the UID stayed on screen.

**Why.** Three independent mistakes, any one of which is enough:

1. **The denominator was wrong.** "Frames" in the status file counts **present
   callbacks**, not displayed frames, and the present callback fires a variable
   number of times per real frame — measured at 2.9× and 5.7× in two sessions.
   The game draws the UID once per real frame, so a batch that is permanently on
   screen scores about 1/2.9 ≈ **34%**. The status file reported 33% for its
   strongest candidate, which is that arithmetic exactly. An 85% bar measured
   against that denominator can only ever be cleared by batches that draw several
   times per present callback — the short bursts belonging to menus, which is
   precisely what the bar was meant to exclude.

2. **One element arrives as several batches.** The index pool the UID is drawn
   from is re-sliced as scenes change, so a single on-screen element is handed
   out as a chain of batches with different `ibo` values, each covering part of
   the session. Judged one at a time, every incarnation is diluted by the gaps
   the others occupy.

3. **The lock was recomputed every frame** rather than remembered. A batch whose
   span kept growing while its draws did not fell back below the bar and was
   silently unlocked again — which is the reported symptom verbatim: the UID
   hidden most of the time, and visible every so often.

**Fixed in v2.1.0**, which is why the criterion is now two questions instead of
one ratio.

## Download

Every [release](../../releases) carries a single file:

| File | Size | Notes |
|---|---|---|
| **`WuwaUID.addon64`** | ~235 KB | The add-on. Drop it in and forget about it. |

The v1.0.0 release additionally carries `WuwaUID-debug-hotkeys.addon64`, a
bisection tool that is **no longer needed or shipped** — v2 needs no manual
bisection. See [Known issues in v1.0.0](#known-issues-in-v100).

v2.1.0 SHA-256:

```
C9A0599214983D3074F29894F0E91EBD4144DCA248B667185963F665D9C8B725  WuwaUID.addon64
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
WuwaUID v4 live status
======================
mode                : automatic (target learned at runtime, no hotkeys)
frames              : 27990
draws seen          : 14542925
draws suppressed    : 8907
  of which from .ini: 0
manual rules        : 0

--- target learning ---
target              : ps=5B44B3683F03BAE3 count=156
draws on target     : 11232
state               : locked on
bar                 : 300 draws, 20% duty, span >= 2400 frames (now)
incarnations adopted: 3
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=29339392 (locked@3280 last@6406)
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=25989888 [inherited] (locked@7077 last@12410)
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=16546048 [inherited] (locked@12777 last@16505)
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=17503232 [inherited] (locked@17275 last@27987)

candidates (bar: 300 draws, 20% duty, span >= 2400):
  [0] inst=1 first=0 voff=0 indexed=1 ibo=17503232   duty  54%  draws  5849  span 17192..27987  solo 4511  gap   3  seg 1649   <-- suppressed
  [1] inst=1 first=0 voff=0 indexed=1 ibo=29339392   duty  33%  draws  1415  span  2171..6406   solo 1414  gap   6  seg  938   <-- suppressed
  [2] inst=1 first=0 voff=0 indexed=1 ibo=25989888   duty  21%  draws  1212  span  6847..12410  solo 1141  gap 634  seg 1073   <-- suppressed
  [3] inst=1 first=0 voff=0 indexed=1 ibo=16546048   duty  23%  draws   912  span 12562..16505  solo  532  gap 728  seg  769   <-- suppressed
  [4] inst=1 first=0 voff=0 indexed=1 ibo=18212352   duty 100%  draws   170  span 26286..26455  solo    0  gap   0  seg    0
  [5] inst=1 first=0 voff=0 indexed=1 ibo=8732416    duty  24%  draws   162  span 15591..16239  solo    0  gap   7  seg  161
  [6] inst=1 first=0 voff=0 indexed=1 ibo=20349440   duty  19%  draws   158  span 23099..23922  solo    0  gap 203  seg  154

--- per-frame match count ---
  frames with 0 matches : 18603
  frames with 1 match   : 7598
  frames with 2+ matches: 1789

--- diagnostics ---
pipelines seen      : 6708 (with pixel shader: 4279)
pipeline binds      : 6297772 (PS hash resolved: 6225669)
command lists       : 64
draws w/o bound PSO : 0
```

| Reading | Meaning |
|---|---|
| `state : locked on` | At least one target has been chosen and is being suppressed |
| `state : warming up` | Too few target draws yet to decide — normal for the first seconds |
| `state : no candidate yet` | Nothing clears the bar right now; the UID is visible |
| `draws suppressed` above `0` | Draws are actually being dropped. If this stays at `0`, nothing is hidden |
| `incarnations adopted: 3` | Three further batches were recognised as the same on-screen element re-sliced — the `[inherited]` rows above |
| `solo` on a suppressed row | Frames in which this batch was the **only** candidate drawn: the second criterion, and the one a menu can never satisfy |
| `duty 100%` on an *unsuppressed* row | A short burst — every frame it appears on, and then it is gone. It never reaches 300 draws, and is correctly ignored |
| `draws w/o bound PSO` is not `0` | ReShade is not reporting pipeline binds — please open an issue |
| `with pixel shader` is `0` | Wrong build loaded |

**The first seconds are the visible-cost part.** Nothing is suppressed until a
batch has earned it, so the UID is on screen briefly after launch, and briefly
again after a scene change that hands it a batch which has not been seen before:

| Situation | Cost |
|---|---|
| First lock after launch | about 1,100 present callbacks after the UID's first batch appears |
| Each later scene change | adopted within a few hundred callbacks of the new batch appearing, provided a previously locked one was active in the last 2,400 |

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
     remaining fields. A candidate accumulates its own totals: draws, the frame
     range it has been seen over, and how many of those frames it was the
     **only** candidate drawn on.
   - A candidate is suppressed when it satisfies **one of two independent
     criteria**, and once it does it stays suppressed for the rest of the
     session. Neither criterion is a ratio over a moving window — both describe
     the whole life of the batch, so neither can be argued into or out of an
     answer by what happened in the last three seconds.

     | Criterion | Bar | What it means |
     |---|---|---|
     | **Endurance** | ≥ 300 draws, a span of at least `max(1200, 12% of elapsed)` present callbacks capped at 2400, and at least 20% of those callbacks carrying a draw | The batch has been there for most of the session so far. The 12% term keeps the bar meaningful early on; the cap stops a long session from demanding a batch that has existed for forty minutes. |
     | **Exclusivity** | a span of ≥ 600 callbacks, ≥ 300 solo frames, and solo frames ≥ 25% of the batch's own draws | The batch is drawn at times when nothing else of its kind is. This is the fast path — on the recorded session it locked in about 7 seconds where endurance alone needed 14 — and it is the one a menu can never satisfy, because a menu is only ever open *on top of* the HUD. |

   - **Why two.** Endurance on its own is slow; exclusivity on its own has a
     hole, because two batches that overlap in time can each accumulate solo
     frames during the other's gaps. The duty floor under endurance is what
     stops a long-lived but rarely drawn batch from qualifying on span alone.
     Measured on a live session the separation turned out to be categorical
     rather than marginal: the UID's incarnations scored 532–4,511 solo frames,
     and **every menu burst scored exactly 0**.

   - At most **8** batches are suppressed at once, and a lock is **remembered**
     rather than recomputed. Once a batch is confirmed it stays confirmed until
     it has been idle for 36,000 callbacks — about seven minutes of play.

   - **Incarnations.** The index pool the UID is drawn from is re-sliced as
     scenes change, so the same on-screen element reappears as a new batch with
     a new `ibo`. A new batch is **adopted** — locked without re-earning the bar
     — when it draws on a callback where it is the *only* candidate, has been
     drawn at least 60 times, and a previously locked batch was active within
     the last 2,400 callbacks. The status file marks these rows `[inherited]`
     and counts them. This is what keeps the UID hidden across a scene change
     instead of making the learner start over.

   Nothing here needs to know what a real frame is, and it deliberately does not
   try. The present callback fires a variable number of times per displayed frame
   (measured at 2.9× and 5.7× in two sessions), so "frames" in the status file
   are really callbacks. Every threshold above is either a count of callbacks or
   a count of draws — never a rate against wall time, which is the mistake that
   [v2.0.0 made](#known-issues-in-v200).

5. **Suppress.** The callback returns `true`. From the ReShade documentation:

   > *To prevent this command from being executed, return `true`, otherwise
   > return `false`.*

   The draw never enters the command queue. This is a supported API behaviour,
   not a hack.

## Tuning

Every threshold in step 4 is a `constexpr` at the top of
[`src/main.cpp`](src/main.cpp) and is meant to be edited:

```cpp
// shared
constexpr uint64_t kMinHits         = 300;    // draws before any lock is allowed
constexpr uint64_t kMinDutyPct      = 20;     // of the span, how many callbacks carry a draw
constexpr size_t   kMaxTargets      = 8;      // batches suppressed at once, at most
constexpr uint64_t kStaleFrames     = 36000;  // idle callbacks before a lock is forgotten

// endurance
constexpr uint64_t kMinSpanFrames   = 1200;   // 14 s at 84 fps with ~3 callbacks per frame
constexpr uint64_t kMinSpanSharePct = 12;     // ...or 12% of elapsed, whichever is larger
constexpr uint64_t kMaxRequiredSpan = 2400;   // cap, so a long session stays reachable

// exclusivity
constexpr uint64_t kFastSpanFrames  = 600;    // 7 s
constexpr uint64_t kMinSoloFrames   = 300;
constexpr uint64_t kMinSoloSharePct = 25;     // of the batch's own draws

// incarnation handover
constexpr uint64_t kMinAdoptDraws   = 60;
constexpr uint64_t kHandoverGap     = 2400;
```

Lowering `kMinSoloFrames` or `kMinSoloSharePct` hides the UID sooner and makes it
likelier to briefly suppress the wrong batch first. Lowering `kMinHits` looks
tempting and does not work: replayed against a recorded session it let a 159-draw
menu burst through the exclusivity path, because that burst's span fully
overlapped a UID incarnation drawn far less often and collected solo frames in
its gaps. **Scale, not proportion, is the robust discriminator.** The status file
is the feedback loop — it prints every candidate with its draws, span, duty and
solo count, so a mis-tuned bar shows up as a number rather than a guess.

## When it stops working

v2 has no baked-in fingerprint, so a game update that shuffles shaders or
rebuilds the UI text pool does not break it — the worst case is that it takes a
few more seconds to settle, and `ps=` in the status file changes value.

What *would* break it is a change in the structure of the problem rather than its
constants: if the game stopped drawing the UID as its own batch, or started
drawing the menu labels in the same batch as the UID, then no ranking of
candidates could tell them apart. **That is the known structural weakness of
this approach** — it depends on the UID and the labels staying separable batches,
and on the labels never being the only thing drawn on a callback.

If the UID reappears, the status file says which part failed:

| Symptom | What it means |
|---|---|
| `draws suppressed` stays at `0`, `state` never leaves `no candidate yet` | Nothing is clearing either bar. Every candidate in the table shows why: look for `draws` under 300, or `solo` at 0 |
| The lock happens late, or the UID returns after a scene change | `incarnations adopted` stays at `0` — the handover did not fire, probably because no locked batch was active within `kHandoverGap` callbacks |
| `draws w/o bound PSO` is not `0` | ReShade stopped reporting pipeline binds; nothing in this add-on can work without them |
| Buttons blank again | The structural assumption broke — labels are being drawn as part of the UID's own batch |

`F9` dumps the last 900 callbacks to `WuwaUID.snapshot.txt`: for each one, how
many candidates matched and the full fingerprint of each. That file is what the
two criteria were tuned against, and it is what an issue report should carry.

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
