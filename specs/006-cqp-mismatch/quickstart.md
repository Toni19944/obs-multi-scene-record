# Quickstart: verifying rate-control coherence across editor, log, and shared-encoder consumer

This is the manual verification procedure for feature 006. The plugin has no automated test harness; verification is by inspection of:

- the slot start log line emitted to OBS's current log (Help → Log Files → View Current Log),
- the per-slot editor dialog (Add or double-click a row),
- the on-disk scene-collection JSON (the `multi_scene_record.slots[]` array; see Test 6).

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with at least one scene (call it `Scene A`).
- An encoder that supports both CQP and CBR (NVENC, AMF, or QSV typically; on a CPU-only machine, `obs_x264` supports CRF + CBR, which works analogously).
- OBS log window open so warnings surface immediately.

## Reporting template

```
Test:           [test number and title]
Build:          [git rev-parse HEAD]
OBS version:    [OBS Help → About]
Hardware:       [CPU / GPU]
Pre-fix value:  [observed before this feature]
Post-fix value: [observed after this feature]
Verdict:        PASS / FAIL [+ notes]
```

Save reports under `specs/006-cqp-mismatch/results/` (gitignored; institutional memory).

## Test 1 — US1: consumer slot reports the encoder it actually uses

**Goal**: confirm FR-001 / FR-002 / FR-011 hold for the headline bug.

1. Add slot `A` with NVENC (or AMF/QSV), CQP, value `23`. Save.
2. Add slot `B` with the same encoder, CBR, value `6000`. Save.
3. Edit slot `B`, change the video encoder to `Use slot A's encoder`. Save. (This is the "switch to consumer" step.)
4. Start slot `B` (per-row state toggle, or its hotkey).
5. Inspect the latest `[multi-scene-rec] 'B' started (...)` log line.

**Pre-fix expectation**: log reports `CBR/6000` (B's stale fields).

**Post-fix expectation**: log reports `CQP/23 inherited from 'A'` (A's owner values).

**Pass criteria**: the rate-control segment matches `CQP/23` (not `CBR/6000`), and the line ends with `inherited from 'A'`.

## Test 2 — US1 Acceptance #3: fallback values reported

**Goal**: confirm FR-003 (fallback values are reported, not the originally requested ones).

1. Configure slot `A` with an encoder that you can make unavailable (e.g., NVENC on a machine without an NVIDIA GPU; or temporarily rename the NVENC plugin DLL).
2. Set A's rate control to CQP/23.
3. Add slot `B` reusing A's encoder.
4. Start B.
5. Inspect B's start log line.
6. Open B's editor.

**Pass criteria**:

- B's log line starts with `[CBR fallback] CBR/6000` (the safe fallback in `slot.cpp:315-316`) and ends with `inherited from 'A'`.
- B's editor shows the rate-control row as `Rate control (inherited from A) [CBR fallback]` and value `6000` (read-only).
- Restore the encoder (e.g., rename the DLL back) before continuing other tests.

## Test 3 — US2 Acceptance #1: switching to consumer does not leave standalone stale values

**Goal**: confirm FR-006 / Decision 2 (sentinel) — the on-disk save is normalized.

1. Add slot `B` with CBR/6000 standalone (any encoder).
2. Save the scene collection (`File → Save`).
3. Open the scene collection JSON file from disk (OBS profile → scene collection folder). Find the slot named `B`.
4. Confirm `rate_control` is `"CBR"` and `rc_value` is `6000`. (This is the pre-switch state.)
5. In OBS, edit slot `B` and switch its encoder to `Use slot A's encoder`. Save the dialog. Save the scene collection again.
6. Re-open the same JSON file. Find slot `B`.

**Pre-fix expectation**: B's `rate_control` is still `"CBR"`, `rc_value` is still `6000` — they look like in-effect values to any external reader.

**Post-fix expectation**: B's `rate_control` is `"<inherited>"`, `rc_value` is `0`.

**Pass criteria**: B's persisted fields are the sentinel + 0.

## Test 3b — FR-016: switching back from shared to a real encoder seeds valid defaults

**Goal**: confirm FR-016 — when the user re-selects a real (non-shared) encoder in the editor for a slot that was previously a consumer, the editor seeds the now-re-enabled rate-control rows with valid defaults for the newly-selected encoder so the saved standalone values are not the `"<inherited>"` sentinel.

1. Start from the post-Test-3 state: slot `B` is a consumer of slot `A`, persisted as `rate_control = "<inherited>"`, `rc_value = 0`.
2. In OBS, edit slot `B`. Without first clicking Save, change the video encoder combo from `Use slot A's encoder` to a real encoder (e.g., NVENC, x264, or whichever the test machine exposes).
3. Observe the now-re-enabled `Rate control` and `Value` rows BEFORE clicking Save.
4. Click Save. Save the scene collection. Re-open the JSON file and find slot `B`.

**Pass criteria**:

- The `Rate control` combo shows the first mode the newly-selected encoder reports (typically `CBR` for NVENC / x264; whichever the encoder lists first). The sentinel `"<inherited>"` is NOT in the combo's selectable items.
- The `Value` spinbox shows the midpoint of that mode's introspected range (e.g., x264 CBR with reported range `[50, 320000]` kbps would seed approximately `160025`; exact value depends on libobs reporting — the rule is midpoint of `[min, max]`, clamped to the range).
- For a lossless mode (e.g., NVENC Lossless if the user then picks it via the combo), the value field shows `— (lossless)` and the underlying value is `0`.
- If the encoder reports no range for the seeded mode, the spinbox is left at its current minimum and the mode is still seeded — the editor does not refuse to render and the sentinel is not exposed.
- After clicking Save and saving the scene collection, B's persisted `rate_control` is the (possibly user-edited) standalone mode and `rc_value` is the (possibly user-edited) standalone value — never the `"<inherited>"` sentinel.

## Test 4 — US2 Acceptance #2: editor shows inherited values read-only

**Goal**: confirm FR-005 (inherited rows, labeled by owner name).

1. With slots A (CQP/23 owner) and B (reuses A) configured from Test 1, open B's editor.
2. Inspect the `Rate control` and `Value` rows.

**Pass criteria**:

- Both rows are **visible**, **disabled**, and **labeled** as `Rate control (inherited from A)` and `Value (inherited from A)` (or similar).
- The combo shows `CQP`; the spinbox shows `23`.
- If A is currently in fallback (Test 2), both labels also include ` [CBR fallback]` and the displayed values are the fallback values.

## Test 5 — US2 Acceptance #3: existing on-disk stale saves are migrated at load

**Goal**: confirm FR-012 — pre-006 saves with stale consumer fields are normalized at load.

1. Quit OBS.
2. Hand-edit the scene collection JSON: on slot `B` (the consumer), set `rate_control` to `"VBR"` and `rc_value` to `9000`, leaving `shared_encoder_slot_id` as the owner's id.
3. Save the JSON and re-launch OBS.
4. Open OBS's current log immediately. Verify there is **no** warning about B's mode/value being clamped or substituted (consumer-side normalization is silent — it doesn't run the clamp/substitute path; it just clears).
5. Open B's editor and inspect the rate-control rows.
6. Save the scene collection (`File → Save`) without changing anything in B. Re-open the JSON.

**Pass criteria**:

- B's editor shows the inherited values from A (not `VBR / 9000`).
- After the save, B's persisted `rate_control` is `"<inherited>"`, `rc_value` is `0`.
- A's stored values are untouched.

## Test 6 — FR-013: out-of-range value is clamped at load with one warning

**Goal**: confirm Decision 3 / FR-013.

1. Quit OBS.
2. Hand-edit the scene collection JSON: on an **owner** slot `A` (with CQP mode), set `rc_value` to `99` (outside CQP's typical 0–51 range on NVENC).
3. Save the JSON and re-launch OBS.
4. Open OBS's current log.
5. Open A's editor.
6. Save the scene collection. Re-open the JSON.

**Pass criteria**:

- Exactly one warning line per slot at load, of the form `[multi-scene-rec] 'A': rc value 99 out of range for CQP on jim_nvenc [0, 51]; clamped to 51`.
- A's editor shows `51` in the value spinbox.
- Starting A logs `CQP/51` in the slot start line.
- After the save, A's persisted `rc_value` is `51`.

## Test 6b — FR-013: out-of-range bitrate value is clamped at load with one warning

**Goal**: confirm FR-013's bitrate-range branch (Test 6 covers the quality-range branch only; FR-013 explicitly applies to bitrate-based modes too).

1. Quit OBS.
2. Hand-edit the scene collection JSON: on an **owner** slot `A` configured with a bitrate-based mode (e.g., `rate_control = "CBR"` on any encoder), set `rc_value` to `999999` — a value clearly above the encoder's reported bitrate ceiling. (Verify the encoder's ceiling first; for `obs_x264` it is typically 320000 kbps; for NVENC it is typically 800000 kbps.)
3. Save the JSON and re-launch OBS.
4. Open OBS's current log.
5. Open A's editor.
6. Save the scene collection. Re-open the JSON.

**Pass criteria**:

- Exactly one warning line per slot at load, of the form `[multi-scene-rec] 'A': rc value 999999 out of range for CBR on <encoder id> [<min>, <max>]; clamped to <max>`.
- A's editor shows the clamped value in the spinbox.
- Starting A logs the clamped value in the slot start line (e.g., `CBR/320000`).
- After the save, A's persisted `rc_value` is the clamped value.

## Test 7 — FR-015: invalid mode is substituted at load with one warning

**Goal**: confirm Decision 4 / FR-015.

1. Quit OBS.
2. Hand-edit the JSON: on an owner slot `A` with NVENC, set `rate_control` to `"XYZ"` (a mode no encoder lists).
3. Save and re-launch.
4. Open OBS's current log.
5. Open A's editor.
6. Save the scene collection. Re-open the JSON.

**Pass criteria**:

- One warning line: `[multi-scene-rec] 'A': rate-control 'XYZ' not supported by jim_nvenc; substituted 'CBR'` (or whichever mode the encoder lists first — typically CBR).
- Editor shows the substituted mode.
- If the existing `rc_value` is out of range under the substituted mode, a second warning emits per Test 6.
- Starting A logs the substituted mode.
- Persisted form on save matches the substituted mode.

## Test 8 — FR-009: owner reconfigure propagates on next consumer start

**Goal**: confirm propagation across edit cycles.

1. With A (CQP/23) and B (consumer of A) configured, stop both.
2. Edit A: change CQP from 23 to 18. Save dialog.
3. Start B.

**Pass criteria**: B's start log reports `CQP/18`, not `CQP/23`. The editor for B (opened any time after the change) shows `CQP/18` in the inherited row.

## Test 9 — Edge case: Lossless

**Goal**: confirm FR-008.

1. Edit A: choose Lossless (an encoder that supports it; e.g., NVENC).
2. Start A.

**Pass criteria**: A's start log reads `... Lossless, ...` (no numeric value). The editor for A continues to show `— (lossless)` in the value row.

If B (consumer of A) is also started: B's start log reads `... Lossless ... inherited from 'A'`.

## Test 10 — FR-007: editor range and encoder-build key alignment per encoder

**Goal**: confirm Decision 7 / FR-007.

For each of the following encoders, when the plugin is built on a machine that has it:

| Encoder | Quality mode | Action |
|---|---|---|
| `obs_x264` | CRF | enter value 22; start; confirm log says `CRF/22` and the encoded file's bitrate matches CRF 22 expectations |
| `jim_nvenc` (or platform-equivalent NVENC) | CQP | enter value 23; start; confirm log says `CQP/23` |
| `h264_texture_amf` | CQP | enter value 23; start; confirm log says `CQP/23` |
| `obs_qsv11_h264` | CQP (split keys) | enter value 23; start; confirm log says `CQP/23` |
| `vt_h264_*` | quality | enter value 50; start; confirm log says `<mode>/50` |
| `aom_av1` / `svt_av1` | CRF | enter value 28; start; confirm log says `CRF/28` |

**Pass criteria**: in every case, the value the user typed in the editor is the value the log reports. No silent clamping caused by a range / key mismatch; no warning about clamping when the value is within the range.

If a particular encoder is unavailable on the test machine, document "not tested on this machine" and skip — the contract is verified by the source factoring (one shared `quality_keys()` list), and any encoder where the source build references it is covered.

## Test 11 — Regression check: standalone slots still work as before

**Goal**: confirm no regression for the existing common case.

1. Add a standalone slot `C` with NVENC, CBR/8000.
2. Start C.

**Pass criteria**: C's start log reads `CBR/8000` (no `inherited from`, no `[CBR fallback]`). The editor remains fully editable.

## Test 12 — Regression check: shared-encoder runtime semantics (constitution VIII)

**Goal**: ensure literal-semantics of the shared encoder is preserved.

1. With A (CQP/23) and B (consumer of A) configured:
2. Start B first (A is stopped).
3. Start A.
4. Stop A. (Constitution principle II: stopping the owner must not stop B.)
5. Stop B.
6. Start A only. Stop A. Start B only. Stop B.

**Pass criteria**: B records correctly throughout. Stopping A does not stop or disturb B. The encoder used by B is the same `obs_encoder_t*` from A's encoder context (verifiable via the absence of any `obs_video_encoder_create` log for B beyond the initial group build). Existing behaviour from features 002 / 003 is preserved.

## Test 13 — FR-010: consumer starts while owner is stopped

**Goal**: confirm FR-010 — the values reported by a consumer's start log MUST come from the owner's persisted configuration regardless of whether the owner is currently running, including the case where no `SharedEncoder` row exists yet.

1. With A (CQP/23 owner) and B (consumer of A) configured per Test 1, ensure both slots are stopped. Quit OBS and re-launch so no `SharedEncoder` row exists in `SlotManager::shared_` for A's id.
2. Without starting A, start B alone.
3. Inspect B's start log line.
4. Open B's editor (close and reopen if it was already open) and inspect the inherited rows.

**Pass criteria**:

- B's start log line reports `CQP/23 inherited from 'A'` — the owner's persisted value, even though A's `SharedEncoder` was not built before B started.
- B's editor inherited rows show `Rate control (inherited from A)` = `CQP` and `Value (inherited from A)` = `23` with NO `[CBR fallback]` suffix (because the helper returns `fallback == false` when no `SharedEncoder` row currently exists OR when one exists with `encoder_fallback_ == false`).
- Stopping B does not affect A or any other slot.
- Re-running the test with A running first and B starting second (the inverse order) yields the same B-side report.
