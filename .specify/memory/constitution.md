<!--
Sync Impact Report
==================
Version change: 1.2.0 → 1.3.0
Modified principles:
  - VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE): added a
    "Host-resource precondition" sub-section that scopes the principle's
    MUSTs to the case where the host can allocate the required resources,
    and admits feature-agnostic degraded operation under three conditions
    (loud surfacing, slot-local confinement, non-silent at save time).
    Silent degradation remains ship-blocking. No MUST removed.
Added sections: None (sub-section added inside Principle VII)
Removed sections: None
Templates:
  - .specify/templates/plan-template.md  ✅ Constitution Check is generic; no change required
  - .specify/templates/spec-template.md  ✅ No constitution-specific references; no change required
  - .specify/templates/tasks-template.md ✅ No constitution-specific references; no change required
  - .specify/templates/commands/         ✅ No command files present
Deferred TODOs: None

Previous report (v1.1.0 → v1.2.0):
  Added Development Workflow bullet "Patch notes" requiring a CHANGELOG-style
  Markdown file at repo root, with an entry per feature and per bug fix.

Previous report (v1.0.0 → v1.1.0):
  Added Core Principles VI–IX (Pipeline Isolation, Recording & Replay Buffer
  Correctness [NON-NEGOTIABLE], Shared Encoder Literal Semantics,
  Configurable Settings Parity) and the Product Quality Bar section.
-->

# Multi Scene Record Constitution

## Core Principles

### I. Native OBS API Compliance

The plugin MUST use only the public interfaces exposed by `libobs` and
`obs-frontend-api`. Direct access to OBS internal headers or unexported
symbols is forbidden. Where OBS source modifications are required, they are
committed in-tree alongside the plugin source.

**Rationale**: OBS makes no stability guarantees for internal APIs. Coupling
to internals creates a maintenance burden on every OBS update and breaks
cross-platform portability.

### II. Clear Ownership & Minimal Shared State

Each `SceneSlot` owns exactly one recording pipeline (outputs + audio
encoders). `SharedEncoder` — the shared video-encoder context — is owned
exclusively by `SlotManager` and leased to consumer slots via an explicit
reference-counted acquire/release protocol.

Owner and sharing slots are **symmetric consumers**: the `SharedEncoder`
context is built on the first consumer's acquire and destroyed only when the
last consumer releases. Stopping the owner slot MUST NOT stop or disturb any
sharing slot. The owner is not required to be running for a sharing slot to
run. No slot MUST reach into another slot's pipeline directly; cross-slot
data flows through `SlotManager` only.

**Rationale**: Symmetric consumer semantics make slot lifecycle independent
and predictable. Explicit ownership prevents double-free and ensures stopping
one slot cannot corrupt a sibling's recording pipeline.

### III. Thread Safety (NON-NEGOTIABLE)

Every mutable shared resource MUST be guarded by an explicit mutex. The
global lock order MUST be observed on every call path:

```
SlotManager::mtx_ → SceneSlot::slot_mtx_ → SceneSlot::stats_mtx_ → SlotManager::shared_mtx_ (leaf)
```

`shared_mtx_` is a strict leaf: code holding it MUST NOT acquire any other
lock. Hotkey callbacks (main thread) and output-stop signals (libobs worker
thread) MUST be safe under concurrent slot start/stop. Deadlocks are blocking
defects; lock-order violations MUST be fixed before merge.

**Rationale**: OBS fires callbacks on worker threads. Incorrect locking
causes data races that corrupt recording pipelines or crash OBS.

### IV. UI / Logic Separation

Qt widget code (`ui-dock`, `ui-slot-editor`) MUST NOT call libobs or
`obs-frontend-api` directly. All OBS operations MUST route through
`SlotManager`. Widgets read `SlotManager` state; they MUST NOT own or store
OBS handles. The dock refreshes on UI-thread signals only; it MUST NOT
block on worker threads.

**Rationale**: Mixing Qt widget code with raw OBS calls couples the UI to OBS
object lifetime, causing crashes on module unload and making the UI logic
untestable in isolation.

### V. Encoder Robustness & Graceful Fallback

The plugin MUST degrade gracefully when a requested encoder is unavailable.
The x264/CBR fallback path MUST set `encoder_fallback_` on both
`SharedEncoder` and the consuming `SceneSlot`, and MUST surface the
`[CBR fallback]` indicator in the UI. `SharedEncoder` settings are fixed at
creation time and MUST NOT change while any consumer slot is attached.

**Rationale**: Users may not have hardware encoders. Silent failure stops
recordings unexpectedly; the UI indicator ensures users are aware of any
encoder substitution.

### VI. Pipeline Isolation From OBS Main

The plugin's recording and replay-buffer pipelines MUST remain fully
independent from OBS's main streaming/recording pipeline. Starting,
stopping, reconfiguring, or crashing any plugin slot MUST NOT affect OBS's
primary stream, primary recording, or primary replay buffer — and starting,
stopping, or reconfiguring those primary outputs MUST NOT disturb any
plugin slot. The plugin MUST NOT reuse, mutate, replace, or take ownership
of OBS main `obs_output_t` handles, nor share an encoder context with the
main pipeline.

**Rationale**: Users rely on the OBS main outputs being authoritative and
untouched. Any cross-contamination undermines the plugin's value
proposition (additive, parallel capture) and risks losing user-critical
streams or recordings.

### VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE)

Every slot's recording output and replay buffer MUST behave exactly per
its configured parameters:

- Slots configured with different replay-buffer lengths MUST be able to
  run simultaneously without interfering with one another's buffer
  contents, save triggers, or output files.
- Both **record + replay** mode and **replay-only** mode MUST be fully
  supported per slot, independently of any other slot's mode.
- A replay save MUST produce a file containing the slot's configured
  duration, regardless of which other slots are currently running, saving,
  or stopping.
- Stopping one slot MUST NOT truncate, drop frames from, or otherwise
  corrupt any other slot's in-flight recording or buffered replay.

**Host-resource precondition**: The MUSTs above assume the host can
allocate the resources (RAM, disk bandwidth, GPU encode capacity, etc.)
required by the slot's configured parameters. When a host-resource
shortfall prevents honoring a configured parameter, the plugin MAY
degrade the affected slot's behavior provided **all** of the following
hold:

1. The degradation is detected and surfaced loudly — a warning in the
   plugin log (and, where a UI surface exists, in the UI) naming the
   slot, the requested value, the value actually in effect, and at least
   one actionable remedy the user can apply.
2. The degradation is confined to the slot whose resources could not be
   satisfied; sibling slots' configured parameters remain honored.
3. The degradation is not silent at save/output time — any artifact
   produced under degraded conditions (e.g., a replay save shorter than
   configured) MUST be accompanied by log wording that identifies the
   shortfall as the cause, not misattributed to a different failure.

Silent corruption, silent truncation, and unobservable degradation
remain ship-blocking defects. The MUSTs apply unconditionally when the
host-resource precondition holds.

**Rationale**: These are the plugin's headline features. Any regression
here is a ship-blocking defect — a multi-scene recorder that drops frames
when a sibling stops, or saves the wrong duration without telling the
user, has failed at its primary job. Host-resource exhaustion is a real
boundary condition the plugin cannot eliminate; what the plugin CAN do is
refuse to lie about it.

### VIII. Shared Encoder — Literal Semantics

When a slot is configured to use another slot's encoder (e.g., "Use Slot 1
Encoder"), it MUST encode exclusively with the video-encoder context owned
by that named slot at acquisition time, with that slot's exact encoder
settings. There MUST be no silent substitution to a different encoder, no
implicit parameter override, and no reconfiguration of the shared context
while any consumer is attached.

Encoder-fallback substitution (see Principle V) is the sole exception and
MUST be surfaced in the UI on every affected slot; it MUST never silently
downgrade the settings of an otherwise-working encoder.

**Rationale**: The shared-encoder feature exists to guarantee identical
encoded output across slots and to save GPU/CPU cycles. Any deviation from
literal semantics defeats both purposes and erodes user trust in the
feature's label.

### IX. Configurable Settings Parity

Every user-configurable setting currently exposed in the plugin UI MUST
remain available and functional in the shipped product. This includes, but
is not limited to: audio-track selection, container/format selection,
encoder selection, filename templates, replay-buffer length, output path,
and shared-encoder mode. Settings MAY be relocated, regrouped, or renamed
for clarity, and defaults MAY be adjusted, but no setting MAY be silently
removed or rendered inoperative.

**Rationale**: Existing users have workflows built on the current setting
surface. Removing a knob without a documented migration path breaks those
workflows and is a regression even if the new behaviour is "better."

## Product Quality Bar

The end goal is a flawlessly functioning and polished product, shippable
with a clear conscience, whose stability and performance are on par with
the most popular and most well-regarded OBS plugins. To reach that bar:

- **Open scope for quality work**: No code path is out of scope for
  bug-fixing, performance improvement, stability improvement, or quality-
  of-life enhancement. Refactors required to fix a defect or eliminate a
  performance/stability cliff are in-scope, provided they preserve all
  other principles in this constitution.
- **No known-broken ship**: A release MUST NOT include a defect that
  violates any NON-NEGOTIABLE principle (III, VII). Known defects in other
  areas MUST be documented in the release notes.
- **Performance baselines**: Plugin overhead in idle (no slot running)
  MUST be negligible relative to OBS itself. Per-slot CPU/RAM cost MUST
  scale linearly with slot count under steady-state recording.
- **Polish**: UI strings MUST be free of debug-only text in shipped
  builds. Log spam at default verbosity is a defect. Crash-on-unload is a
  ship-blocking defect.

## Build & Platform Requirements

- **Language**: C++17 minimum. C++20 features MUST NOT be introduced unless
  all three CI toolchains — MSVC 2022, Clang/AppleClang (Xcode 16), GCC
  (Ubuntu 24.04) — support them unconditionally.
- **CMake**: 3.28 minimum; 3.30 maximum as declared in `cmake_minimum_required`.
- **Qt**: Qt 6 Widgets + Core only. Qt 5 compatibility shims are forbidden.
- **OBS**: Studio 31.1.1+ (pinned in `buildspec.json`). Builds MUST use the
  pre-built deps bundle from `obsproject/obs-deps`.
- **Target platforms**: Windows x64 (Visual Studio 17 2022), macOS
  (Xcode 16.0), Ubuntu 24.04 (ninja-build, pkg-config, build-essential).
- **CMake automation**: `AUTOMOC`, `AUTOUIC`, and `AUTORCC` MUST be enabled
  on the plugin target. Manual `moc`/`uic` invocations are forbidden.

## Development Workflow

- **Versioning**: Semantic versioning (`MAJOR.MINOR.PATCH`). The `version`
  field in `buildspec.json` is the canonical source of truth. A release is
  triggered by pushing a semantically-versioned tag (e.g., `1.2.3`) to
  `master`/`main`.
- **CI pipelines**: GitHub Actions. `push` on `master`/`main` runs
  `build-project` and `check-format`. Pull requests (`pr-pull`) run the same
  gates. `dispatch` allows manual triggers.
- **Formatting gate**: CMake and C++ source formatting is checked by the
  `check-format` workflow. All contributions MUST pass formatting before merge.
- **OBS modifications**: Changes to OBS source MUST be committed in-tree
  alongside the plugin source. There is no patch-file workflow.
- **Commit hygiene**: Each logical change is its own commit. Force-pushes to
  `master`/`main` are forbidden.
- **Patch notes**: A Markdown patch-notes file MUST exist at the repository
  root (canonical name: `CHANGELOG.md`). Every new feature and every bug fix
  MUST add an entry to that file documenting the user-visible change before
  the work is merged.

## Governance

This constitution supersedes all other practices documented in this project.
When a coding pattern, design decision, or workflow conflicts with a stated
principle, the constitution takes precedence and the conflict MUST be resolved
before the work ships.

**Amendment procedure**:
1. Edit `.specify/memory/constitution.md` with the proposed change.
2. Bump the version following semantic rules:
   - MAJOR — principle removal or redefinition of a non-negotiable rule.
   - MINOR — new principle or section added.
   - PATCH — clarifications, wording, or typo fixes.
3. Propagate the change to all dependent templates (plan, spec, tasks).
4. Record the change in the Sync Impact Report (HTML comment at top of this file).

**Compliance review**: All PRs MUST be reviewed for compliance with the
NON-NEGOTIABLE principles — Principle III (Thread Safety) and Principle VII
(Recording & Replay Buffer Correctness) — and with Principle IV (UI/Logic
Separation). Any design that violates Principle II (Clear Ownership),
Principle VI (Pipeline Isolation), or Principle VIII (Shared Encoder Literal
Semantics) MUST be justified in the plan's Complexity Tracking table before
implementation begins. Any PR that removes a user-configurable setting MUST
cite Principle IX and document the migration in the release notes.

**Runtime guidance**: See `CLAUDE.md` for agent-specific development guidance.

**Version**: 1.3.0 | **Ratified**: 2026-05-19 | **Last Amended**: 2026-05-26
