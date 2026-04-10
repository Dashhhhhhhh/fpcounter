# AGENTS.md

## Mission

This repository exists to build a **trustworthy frame-window analysis tool for Geometry Dash**.

The project is not trying to be flashy first. It is trying to be **correct first**.

Everything in this repo should serve one main goal:

> Given a known-good input, determine how many nearby ticks would also succeed, and report that result clearly, consistently, and with enough logging to verify that it is real.

This repo should feel like a **debug instrument**, not a black box.

---

## Project philosophy

### 1. Debug-first over polish-first
The mod should always prefer visibility over presentation.

A rough overlay with honest numbers is better than a pretty UI with questionable numbers.

A contributor should always ask:

- Can I prove this result is correct?
- Can I inspect why it failed?
- Can I explain the output from logs alone?

If the answer is no, the feature is not ready.

### 2. Deterministic over clever
The system should behave in a way that is stable and inspectable.

Avoid logic that "usually works" but cannot be reasoned about from logs, replay frames, and game state.

This project should prefer:

- explicit state
- explicit tick tracking
- explicit sync checks
- explicit failure reasons

over hidden heuristics or vague fallback behavior.

### 3. Frame-accurate over performance-optimized
Performance matters, but not at the cost of correctness.

A slower correct result is more valuable than a fast wrong result.

Optimization is welcome only after the result is trustworthy.

### 4. Non-platformer only
This repo is for **classic Geometry Dash gameplay only**.

Platformer support is out of scope unless the project direction explicitly changes.

Do not quietly blur the boundary between platformer and non-platformer logic.

### 5. No hidden magic
Contributors should not add silent corrections, hidden offsets, or automatic fallback logic that changes meaning without being visible in logs.

If the system calibrates, offsets, skips, or downgrades anything, it must be logged clearly.

---

## What this repo is trying to measure

The core question is:

> Around a known-correct input, how wide is the successful timing window?

The result should be expressed in simple window terms:

- `1f`
- `2f`
- `3f`
- `4f`
- `5f`
- `>5f`
- `invalid`

### Meaning of results

- **1f**: only one tick works
- **2f-5f**: a contiguous success window of that width exists around the correct input
- **>5f**: the success window is wider than the project's meaningful range
- **invalid**: the system could not confirm a valid success window containing the original input

### Important rule
This repo does **not** care about counting or celebrating anything wider than 5 ticks.

`>5f` may be displayed or logged, but it should not be treated like a meaningful tight frame window.

---

## Core design principles

### Modularity
Keep major concerns separated.

The repo should conceptually stay split into pieces like:

- macro loading / parsing
- live sync and tick tracking
- frame-window analysis
- branch testing
- overlay / UI
- debug logging
- result storage or caching

Avoid giant "do everything" functions.

### Sync correctness comes first
A frame-window result is worthless if the system is not aligned with the macro and gameplay state.

Before adding new behaviors, contributors should verify:

- macro frame numbering is understood
- macro FPS is understood
- live tick counting is stable
- input matching happens on the intended tick boundary
- analysis starts from the true pre-input state

### Branch determinism matters
The analyzer should be built around reproducible branch testing.

When checking offsets around a correct input, each branch should start from a state that is as close as possible to the true pre-input state.

Do not accept desynced branch simulation as "close enough."

### Logging is part of the product
Logs are not secondary. Logs are a required feature.

Any meaningful analysis path should leave behind enough information to answer:

- what input was analyzed
- what the reference tick/frame was
- what offsets were tested
- which offsets succeeded
- why failures happened
- how the final window was chosen

---

## What contributors should consider before adding features

Before implementing anything new, think through these questions.

### Tick sync
- What exactly counts as one live tick?
- Where is the canonical tick boundary?
- Is the analysis happening before input application or after it?
- Is the current tick counter stable across resets and attempts?

### Macro FPS alignment
- Does the macro's frame index map directly to the live tick counter?
- Is there any FPS mismatch?
- Is there any startup offset or calibration happening?
- Is that calibration visible in logs?

### Branch start state
- Are branch checks starting from the real pre-input state?
- Is the baseline branch equivalent to the original successful run?
- Could offset `0` fail because the branch state is already wrong before testing begins?

### Determinism
- If the same macro is analyzed twice, do the same inputs produce the same result?
- Are there hidden conditions that change the result between runs?

### Desync risk
- Is the feature accidentally using already-modified live state?
- Is it relying on a visual frame boundary instead of a gameplay tick boundary?
- Is it hiding sync issues instead of surfacing them?

---

## Logging standard

All significant behavior should use clear, grep-friendly prefixes.

Required prefixes:

- `[MacroParser]`
- `[FrameCheck]`
- `[FrameCheck/Sync]`
- `[FrameCheck/State]`
- `[FrameCheck/Test]`
- `[FrameCheck/Result]`

### Logging expectations

#### `[MacroParser]`
Use for:
- macro load path
- replay metadata
- total input count
- FPS / framerate
- platformer flag
- parse failures
- ignored or unsupported macro conditions

#### `[FrameCheck]`
Use for:
- startup
- level enter
- attempt begin
- attempt reset
- live analyzer enable/disable
- high-level analysis lifecycle

#### `[FrameCheck/Sync]`
Use for:
- live tick vs macro frame relationship
- calibration offsets
- FPS mismatch warnings
- timing-boundary decisions
- any sync warning that could invalidate results

#### `[FrameCheck/State]`
Use for:
- current input index
- counters
- current analysis queue
- state resets
- mode changes or analyzer state changes

#### `[FrameCheck/Test]`
Use for:
- each tested offset
- branch start state
- branch result
- fail reason
- whether the branch survived or diverged

#### `[FrameCheck/Result]`
Use for:
- contiguous window chosen
- final bucket
- offset map
- invalid-result explanation

### Logging rule
Never silently downgrade behavior.

If the code skips an input, rejects a branch, recalibrates timing, or falls back to another method, log it.

---

## What not to do

### Do not add platformer support casually
This repo is non-platformer only unless that scope changes explicitly.

### Do not count beyond 5 ticks
Anything above 5 should be treated as `>5f`, not as a special achievement.

### Do not add silent fallback logic
No invisible "best effort" behavior that changes the meaning of results.

Examples of bad behavior:
- silently changing FPS assumptions
- silently shifting input frames
- silently skipping broken branches
- silently replacing invalid with a guessed result

### Do not ship with universal invalid results
If a known-good completed run produces all `invalid` results, the analyzer is not trustworthy.

That is a stop sign, not a minor issue.

### Do not optimize away observability
Do not remove logs or compress logic so much that failures become hard to diagnose.

### Do not hide uncertainty
When the analyzer is unsure, say so in logs and results.

Honest uncertainty is better than false precision.

---

## Expected development workflow

### 1. Start with known-good macros
New analysis work should be tested against macros that are already known to complete correctly.

### 2. Verify offset 0 first
Before caring about the full frame window, verify that the original correct input succeeds at offset `0`.

If offset `0` fails, the rest of the result is suspect.

### 3. Check sync before tuning logic
When results look wrong, inspect:
- macro FPS
- live tick mapping
- pre-input state
- branch start conditions

Do not jump straight to changing window logic before proving sync is correct.

### 4. Keep changes testable
Every new feature should make the analyzer easier to inspect, not harder.

Prefer changes that:
- add more useful logs
- isolate modules
- reduce ambiguity
- make failure modes clearer

### 5. Treat all-invalid output as a critical bug
A correct macro producing all-invalid results means the analyzer is failing fundamentally.

Do not treat that as acceptable noise.

---

## Contributor checklist

Before opening a PR, confirm:

- The feature stays within non-platformer scope
- The behavior is visible in logs
- Sync assumptions are logged
- Offset `0` succeeds for known-good macro inputs
- Results are classified only as `1f` to `5f`, `>5f`, or `invalid`
- Nothing wider than 5 is counted as meaningful
- No silent fallback logic was introduced
- The code is easier to reason about than before, not harder

---

## Final doctrine

This repo should behave like a **precision instrument**.

It should be skeptical, explicit, and honest.

It should never pretend to know more than it can prove.

When in doubt:

- log more
- sync first
- trust determinism
- keep the scope narrow
- favor correctness over convenience
