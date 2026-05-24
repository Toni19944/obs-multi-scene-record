# Feature Specification: Fix Hotkey Registration

**Feature Branch**: `001-fix-hotkey-registration`

**Created**: 2026-05-19

**Status**: Draft

**Input**: User description: "Current hotkey logic is broken, it was changed from `obs_hotkey_register_frontend(...)` to whatever it is now. It is not working because libobs doesn't natively support creating custom hotkey groups. I want to change this, and use `obs_hotkey_register_output(...)` instead."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Toggle a slot's recording with a hotkey (Priority: P1)

A streamer has configured one or more recording slots and bound a keyboard shortcut to "Toggle Recording" for each slot in OBS Settings > Hotkeys. While playing, they press the shortcut to start recording that slot; they press it again to stop it. The shortcut works whether the slot is currently idle or already recording.

**Why this priority**: This is the core value proposition of per-slot hotkeys and is currently broken. Without it, the only way to start/stop a slot is via the dock UI — defeating the point of having configurable hotkeys at all.

**Independent Test**: Configure one slot, bind a key to its "Toggle Recording" hotkey via OBS Settings, then verify pressing the key starts recording (file appears at the configured path) and pressing it again stops recording (file is finalized). Both states must be reachable from the hotkey alone, without touching the dock.

**Acceptance Scenarios**:

1. **Given** a slot exists and is idle, **When** the user presses its bound "Toggle Recording" key, **Then** the slot begins recording and the dock reflects the active state.
2. **Given** a slot is currently recording, **When** the user presses its bound "Toggle Recording" key, **Then** the slot stops recording and the file is finalized.
3. **Given** two slots are configured with different bound keys, **When** the user presses slot A's key, **Then** only slot A reacts and slot B is unaffected.

---

### User Story 2 - Save a replay clip with a hotkey (Priority: P1)

For each slot that has its replay buffer enabled, the user can bind a keyboard shortcut to "Save Replay". While the slot's replay buffer is running, pressing the shortcut saves the last N seconds to disk.

**Why this priority**: Replay capture is the second core hotkey use case for the plugin and shares the same broken registration path. Fixing one without the other leaves the feature half-working.

**Independent Test**: Enable a slot's replay buffer, start the slot, bind a key to its "Save Replay" hotkey, then press the key while the buffer is active and verify a clip file appears at the configured path.

**Acceptance Scenarios**:

1. **Given** a slot's replay buffer is active, **When** the user presses its bound "Save Replay" key, **Then** a replay clip is written to disk.
2. **Given** a slot's replay buffer is not running (slot stopped, or replay not enabled in config), **When** the user presses its bound "Save Replay" key, **Then** nothing happens and no error is shown to the user.

---

### User Story 3 - Bindings survive restarts, renames, and scene-collection reloads (Priority: P2)

A user spends time assigning carefully chosen key combinations to each slot's hotkeys. Those bindings remain attached to the same slot — by identity, not by name or position — after they restart OBS, rename the slot in the dock, reorder slots, or switch scene collections and back.

**Why this priority**: Without persistence, the hotkey feature is technically functional but practically useless: rebinding after every restart is unacceptable. This was already working in the previous implementation and must not regress.

**Independent Test**: Bind keys to a slot's two hotkeys, restart OBS, then verify in Settings > Hotkeys that the same key combinations are still attached to the same slot. Repeat after renaming the slot and after switching scene collections.

**Acceptance Scenarios**:

1. **Given** a slot has bindings on both hotkeys, **When** OBS is restarted, **Then** the bindings are still attached to that slot.
2. **Given** a slot has bindings on both hotkeys, **When** the user renames the slot in the dock, **Then** the bindings remain attached and the Settings > Hotkeys label updates to the new name.
3. **Given** a slot has bindings on both hotkeys, **When** the user switches scene collections away and back, **Then** the bindings are still attached.

---

### User Story 4 - Hotkeys appear under a clear, recognizable group in Settings (Priority: P3)

When a user opens OBS Settings > Hotkeys, they can locate this plugin's per-slot hotkeys quickly — they are grouped together (and labelled in a way that identifies both the plugin and each individual slot), not scattered through the General list mixed with unrelated entries.

**Why this priority**: This is a usability/discoverability win, not a correctness requirement. The plugin would still be functional if every slot's hotkeys appeared in the General list, but they'd be hard to find as the slot count grows.

**Independent Test**: Configure three slots with distinct names, open Settings > Hotkeys, and verify all six per-slot hotkeys are visually grouped together and each is clearly attributable to its slot.

**Acceptance Scenarios**:

1. **Given** three slots are configured, **When** the user opens Settings > Hotkeys, **Then** all six per-slot hotkeys are grouped together and not interleaved with unrelated entries.
2. **Given** a slot is renamed, **When** the user re-opens Settings > Hotkeys, **Then** that slot's hotkey rows reflect the new name.

---

### Edge Cases

- **Slot is added at runtime**: the new slot's hotkeys appear immediately in Settings > Hotkeys without requiring an OBS restart.
- **Slot is removed at runtime**: the removed slot's hotkey rows disappear from Settings > Hotkeys, and pressing any previously bound key for that slot has no effect.
- **Slot's replay is toggled off in config**: the slot's "Save Replay" hotkey either disappears from Settings > Hotkeys, or it remains visible but is a no-op (this is decided in Assumptions / clarifications).
- **Plugin is loaded before OBS's frontend hotkey system is ready**: hotkey registration is deferred to a frontend-ready event (already the case via `OBS_FRONTEND_EVENT_FINISHED_LOADING` and `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED`); this behavior is preserved.
- **A bound key is pressed for a slot whose recording fails to start** (e.g., encoder unavailable, disk full): the user sees the same failure feedback as starting from the dock UI; the hotkey itself does not silently fail.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Each slot MUST expose two named hotkeys to OBS: "Toggle Recording" and "Save Replay".
- **FR-002**: The "Toggle Recording" hotkey MUST start the slot when it is idle and stop it when it is running — i.e., the same single hotkey covers both transitions.
- **FR-003**: The "Save Replay" hotkey MUST trigger a replay clip save when the slot's replay buffer is currently active, and MUST silently do nothing when it is not (no error dialog, no crash).
- **FR-004**: Per-slot hotkey bindings MUST be tied to a stable slot identity (not to the slot's user-facing name or list position), so the binding survives rename and reorder.
- **FR-005**: Per-slot hotkey bindings MUST be persisted across OBS restarts and across scene-collection switches.
- **FR-006**: Adding a slot at runtime MUST make that slot's hotkeys available in Settings > Hotkeys without requiring an OBS restart.
- **FR-007**: Removing a slot at runtime MUST remove its hotkeys from Settings > Hotkeys and immediately disable any previously bound keys for that slot.
- **FR-008**: All per-slot hotkeys MUST appear grouped together in Settings > Hotkeys (not interleaved with unrelated entries in the General list).
- **FR-009**: The hotkey registration mechanism MUST use a libobs-native grouping facility that libobs supports out-of-the-box — specifically `obs_hotkey_register_output(...)`. The plugin MUST NOT attempt to fabricate a custom hotkey group via a private source or any other workaround, because libobs does not natively support arbitrary custom hotkey groups and the current workaround does not function as intended.
- **FR-010**: Each slot MUST appear in Settings > Hotkeys as its own group labelled `Multi-Scene Record: <slot name>`, where `<slot name>` is the slot's current user-facing display name. The label MUST update when the slot is renamed.
- **FR-011**: Both hotkeys ("Toggle Recording" and "Save Replay") MUST be registered for every slot regardless of whether the slot's replay buffer is enabled. When replay is disabled or inactive, pressing "Save Replay" MUST be a silent no-op (see FR-003). Toggling `replay_enabled` in slot config MUST NOT cause the "Save Replay" hotkey to be unregistered or re-registered, and MUST NOT disturb any existing user binding on it.

### Key Entities

- **Slot**: A user-configured recording target with a stable id (used as the persistence key for hotkey bindings), a user-facing display name (used as the label in Settings > Hotkeys), and an optional replay buffer.
- **Hotkey binding**: The key combination a user has assigned, in OBS Settings > Hotkeys, to a particular named hotkey of a particular slot. Stored by the plugin in its scene-collection-scoped save data, keyed by slot id + hotkey name.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can start and stop a slot's recording using only its bound "Toggle Recording" key, with no clicks in the dock, in 100% of attempts under normal conditions.
- **SC-002**: After an OBS restart, 100% of previously assigned per-slot hotkey bindings remain attached to the same slot.
- **SC-003**: After renaming a slot, its hotkey bindings remain attached and the Settings > Hotkeys label reflects the new name within one open/close cycle of the Settings window.
- **SC-004**: With N slots configured, a user can locate any given slot's two hotkeys in Settings > Hotkeys in under 5 seconds (target: grouped contiguously and labelled by slot name).
- **SC-005**: Pressing the "Save Replay" key for a slot whose replay buffer is not currently active produces zero user-visible errors and zero log-level error messages.
- **SC-006**: Zero plugin crashes attributable to hotkey registration, unregistration, or invocation across the full slot lifecycle (add → rename → start → stop → replay → save-replay → remove → scene-collection switch → OBS restart).

## Assumptions

- The previous, working implementation registered both hotkeys via `obs_hotkey_register_frontend(...)`, which placed them in the General list of Settings > Hotkeys but worked correctly. The current implementation attempts to register against a privately created scene source to achieve grouping, and that approach is broken. The fix returns to a functional, libobs-supported registration path (`obs_hotkey_register_output(...)`) and accepts whatever grouping behavior libobs provides for output-registered hotkeys.
- "Output" in this context refers to the libobs concept of an `obs_output_t` — the plugin already creates per-slot recording and replay outputs as part of normal operation. Whether the hotkey is attached to a long-lived per-slot output object or to the transient recording/replay output is an implementation concern handled in `/speckit-plan`; the contract in FR-002 (toggle works while slot is stopped) is the binding requirement.
- The existing save/load path for per-slot hotkey bindings (the `hk_record` / `hk_save_replay` data arrays already stored in each slot's persisted data) is reused as-is; this change is about registration mechanism, not about the persistence format.
- The current `hotkey_group_source()` helper in `SlotManager` (the lazily created private scene source named "Multi-Scene Record") is removed by this change, including its lifetime management in `init()` / `shutdown()`.
- The frontend-event-driven registration timing (register on `OBS_FRONTEND_EVENT_FINISHED_LOADING` and `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED`, unregister on `*_CHANGING` and `EXIT`) is preserved unchanged.

## Resolved Clarifications

- **Settings > Hotkeys grouping**: each slot gets its own group labelled `Multi-Scene Record: <slot name>` (plugin-prefixed slot name). Captured in FR-010.
- **"Save Replay" when replay is disabled**: both hotkeys are always registered for every slot; "Save Replay" silently no-ops when the replay buffer is not active. Captured in FR-011.
