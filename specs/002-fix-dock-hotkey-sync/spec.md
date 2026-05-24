# Feature Specification: Fix Dock UI sync after hotkey-triggered recording

**Feature Branch**: `002-fix-dock-hotkey-sync`

**Created**: 2026-05-19

**Status**: Draft

**Input**: User description: "Hotkey executed recording does not update the plugin dock UI slot state. Otherwise hotkeys work fine and recording happens correctly."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Dock state column tracks hotkey-triggered recording (Priority: P1)

A user has bound a "Toggle Recording" hotkey to a slot. They press the hotkey to start recording. The plugin dock's per-slot state column immediately shows that slot as active. They press the hotkey again to stop. The state column immediately shows the slot as idle. At no point do they have to click in the dock to make its display match reality.

**Why this priority**: Without this, the dock is silently misleading after every hotkey press — the column claims the slot is idle while a file is being written (or vice-versa). Users who rely on the dock as their source of truth for "what's currently recording" will trust the wrong information and risk leaving recordings running unnoticed or missing the start of a session. The recording itself works, so this is purely a display-correctness bug — but a sharply visible one.

**Independent Test**: Open the dock, configure one slot, bind a key to its "Toggle Recording" hotkey. With the dock visible, press the hotkey. Verify the state column flips to the active indicator within one second, without clicking. Press again; verify it flips back to idle. The button cell, indicator dot, recording filename column, and any other state-dependent dock element must all reflect the new state.

**Acceptance Scenarios**:

1. **Given** a slot is idle and the dock is visible, **When** the user presses the bound "Toggle Recording" key, **Then** the dock's state column for that slot shows the active indicator within 1 second, with no dock click required.
2. **Given** a slot is recording and the dock is visible, **When** the user presses the bound "Toggle Recording" key, **Then** the dock's state column shows the idle indicator within 1 second.
3. **Given** the user presses the hotkey for Slot A but Slot B's row is currently selected in the dock, **When** Slot A toggles, **Then** Slot A's row updates and Slot B's row is unaffected.

---

### User Story 2 - Failed hotkey-triggered start leaves the dock honest (Priority: P2)

If the user presses the "Toggle Recording" hotkey for a slot that fails to start — e.g., missing output directory, encoder unavailable, disk full — the dock state column must reflect the actual post-attempt state (idle), not a fake "recording" appearance.

**Why this priority**: The dock-click path already does this (`on_state_clicked` calls `refresh()` after the start attempt, and `refresh()` reads the slot's true `is_running()` state). The hotkey path needs the same guarantee so that a failed start doesn't silently leave the dock claiming success.

**Independent Test**: Configure a slot with an output directory that doesn't exist (or any other guaranteed-to-fail condition), bind its hotkey, press it. Verify the dock continues to show the slot as idle — no fake active state appears, even briefly.

**Acceptance Scenarios**:

1. **Given** a slot whose start will fail (no path / bad encoder / etc.), **When** the user presses its "Toggle Recording" key, **Then** the dock state column remains "idle" with no transient "active" flicker.

---

### Edge Cases

- **Dock is closed/hidden when the hotkey is pressed**: The state update must be a safe no-op; pressing a hotkey while the dock is not displayed must not crash and must not leak resources. When the user reopens the dock, the displayed state must reflect current reality.
- **Hotkey fired in rapid succession**: Pressing the toggle key many times quickly must not corrupt the dock display or queue up stale refreshes that arrive after the user has moved on. The final dock state must match the slot's final `is_running()` state.
- **External / error-driven recording stop** (disk full, encoder failure, OBS shutting down recording): the dock already refreshes via the output's stop signal handler. This change must not interfere with that existing path.
- **Hotkey pressed during a slot edit / removal / scene-collection switch**: the UI refresh must not race with slot teardown. Concurrent slot mutation and a queued UI refresh must not crash the plugin.
- **"Save Replay" hotkey**: triggers a transient action (writing a clip), not a state transition. The dock state column does not need to update for this. Out of scope.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: After a hotkey-initiated start of a slot, the dock's state column for that slot MUST update to the active state within 1 second, without user interaction with the dock.
- **FR-002**: After a hotkey-initiated stop of a slot, the dock's state column MUST update to the idle state within 1 second.
- **FR-003**: The dock update MUST reflect the slot's actual post-transition `is_running()` state. If a hotkey-initiated start failed, the column MUST NOT show a transient "active" state.
- **FR-004**: The dock update MUST happen safely when the dock is closed or hidden (silent no-op, no crash, no leak).
- **FR-005**: The existing dock refresh on external/error stop (driven by the output's stop signal) MUST continue to function unchanged.
- **FR-006**: The existing dock refresh on dock-button clicks (`on_state_clicked`, `on_start_all`, `on_stop_all`) MUST continue to function unchanged.
- **FR-007**: Rapid repeated hotkey presses MUST leave the dock in a state consistent with the slot's final `is_running()` value — no stale refresh "wins" over a later transition.
- **FR-008**: The "Save Replay" hotkey MUST NOT change dock state column rendering (save_replay is a transient action, not a state transition).

### Key Entities

- **Slot state column**: The cell in the dock's slot table that visually indicates whether the slot is currently recording (active) or not (idle). Includes whatever subordinate visuals depend on that state (button label, indicator dot, recording filename, etc. — all driven by the dock's existing `refresh()`).
- **Hotkey-initiated transition**: A start or stop of a slot's recording triggered by the user's bound "Toggle Recording" key, as opposed to one triggered by a dock click, a Start All / Stop All command, or an external signal.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of hotkey-triggered start/stop transitions update the dock state column within 1 second of the key press.
- **SC-002**: A user can read the dock state column alone to determine whether each slot is currently recording, with the column matching reality in 100% of cases regardless of whether the last transition came from a click, a hotkey, a Start All / Stop All command, or an external/error stop.
- **SC-003**: Zero regressions: dock-click start/stop, Start All / Stop All, external/error stop, and Save Replay continue to behave exactly as before this change.
- **SC-004**: Zero plugin crashes attributable to the hotkey→dock refresh path across the full slot lifecycle (idle hotkey, active hotkey, hotkey while dock closed, hotkey during slot edit/removal, hotkey across scene-collection switch).

## Assumptions

- The dock already exposes a `refresh()` slot that rebuilds the per-slot rows from `SlotManager` state — this is the same mechanism used by `on_state_clicked` (dock button click), `on_start_all`, `on_stop_all`, and the external-stop signal handlers (`on_rec_output_stop`, `on_replay_output_stop`). The fix is to invoke that same `refresh()` from the hotkey callback path, marshalled onto the UI thread via the existing `QMetaObject::invokeMethod(..., "refresh", Qt::QueuedConnection)` pattern.
- Constitution Principle IV (UI / Logic Separation) is preserved: the hotkey callback lives in `src/slot.cpp` and does not directly touch Qt widgets. It posts a queued invocation to the dock via the existing `get_dock()` accessor and `QMetaObject::invokeMethod`. No new direct Qt calls are introduced from non-UI code.
- The hotkey callback fires on libobs's hotkey thread (per the constitution's Principle III note), not the UI thread; queueing the refresh via `Qt::QueuedConnection` is therefore mandatory, not optional.
- The bug is independent of the obs_hotkey_register_output migration from feature 001 — the dock-refresh gap exists in the hotkey callback's body, which is unchanged across that migration.

## Resolved Clarifications

No outstanding clarifications. The user-facing behavior is unambiguous (dock must reflect reality), the technical pattern already exists in the codebase (`QMetaObject::invokeMethod` for external stops), and the scope is tightly bounded (the two hotkey callbacks in `slot.cpp` and only the toggle-recording one needs the refresh; save-replay is correctly excluded).
