# Quickstart — Manual Verification

**Feature**: Replay buffer honors configured duration under quality-based rate control

**Branch**: `008-fix-replay-quality-truncation` | **Date**: 2026-05-25

This file lists the manual verification procedure for the 008 fix. Manual coverage is **Windows-only** (the maintainer's test environment). macOS / Ubuntu builds get the same code path; correctness on those platforms rests on the platform-agnostic sizing arithmetic and on the per-platform RAM-probe being a single-line wrapper per OS — same scoping as features 006 / 007.

Each test references the FRs / SCs / User Stories it exercises (from [spec.md](./spec.md)) and the design decisions it validates (from [research.md](./research.md)). Pre-condition for all tests: build the plugin from this branch (`008-fix-replay-quality-truncation`) against OBS 31.1.1 (`buildspec.json`), and load OBS with the plugin enabled.

The maintainer's reference hardware / content configuration (used in T1, T2, T6, T11):

- Encoder: NVENC H.264, preset P5, full-res-double-pass multipass, high-quality tuning, high profile.
- Rate control: CQP, quality value 17 (very high — peaks at 60+ Mbps in measured testing).
- Resolution / fps: 1920 × 1080 @ 60.
- Source: a modern open-world third-person shooter game (high-motion, complex scenes).

---

## Test 1 — US1 headline bug: CQP-17 1080p60 40s save honors configured duration

**Validates**: US1, FR-001, FR-001b, FR-002, FR-004, SC-001 | **References**: research D1 (quality-mode coefficient), contracts C1 (sizing formula)

1. Create slot **QualityTest**: select the maintainer's reference encoder configuration (NVENC P5 / full-res-double-pass / HQ / high profile, CQP 17), 1920×1080 @ 60, replay enabled, replay length 40 s, container MKV. Leave the "Max replay buffer size" spinbox at its default ("Auto"). Bind save-replay hotkey to F8.
2. Start the slot. Confirm the start log line (FR-005 / contract L1) reports the resolved ceiling — for 1080p60 at the maintainer's calibration this should be approximately **653 MB** (`0.55 × 1920 × 1080 × 60 × 40 × 2 / 8 / 1024 / 1024 + audio`).
3. Run the reference TPS game on a scene with sustained high motion (open-world traversal with rapid camera movement, action sequences). Let the slot run for at least 60 seconds so the replay buffer is fully filled and the EWMA has accumulated samples.
4. Press F8.
5. **Expected**: the saved file in the output directory is approximately 40 seconds long (within one keyframe interval — typically ±2 s at the slot's keyframe interval). Confirm by opening the file in a media player (VLC / mpv) and reading the duration.
6. **Pre-fix regression check**: build the previous-revision binary (`git checkout main`, rebuild), repeat steps 1-5 (the override field won't exist on main; the spinbox row is the only difference). Observe a clip duration well below 40 s — typically 10-30 s depending on what content was on screen, matching the user's repro. Switch back to `008-fix-replay-quality-truncation` and rebuild before continuing.

---

## Test 2 — US1 scene-complexity invariance: low-complexity content same configuration

**Validates**: US1 Acceptance #2 / #3, FR-002, SC-002 | **References**: research D1, contracts C1

1. Use the same slot as Test 1. Restart it if it was stopped.
2. Switch to low-complexity content — a static desktop, a still image, a paused video. Let the slot run at least 60 seconds.
3. Press F8.
4. **Expected**: the saved file is approximately 40 seconds long — within one keyframe interval of the Test 1 result. The duration does NOT differ from Test 1 just because the content was different (the contract is "configured duration honored regardless of scene complexity").

---

## Test 3 — US1 bitrate-mode regression (the existing path must keep working)

**Validates**: FR-003, SC-007 | **References**: research D1 (bitrate branch unchanged), contracts C1

1. Create slot **BitrateTest**: same scene / replay length / container as Test 1, but rate control = CBR, bitrate = 8000 kbps (8 Mbps).
2. Start the slot. Confirm the start log line reports the resolved ceiling — for 8 Mbps at 40 s × 2× margin = `8000 × 40 × 2 / 8 / 1024 + audio ≈ 80 MB`. (This is well under the clamp threshold on any reasonable host.)
3. Run any content (complexity doesn't matter at CBR). Wait ≥ 60 s.
4. Press F8.
5. **Expected**: the saved file is approximately 40 seconds long. The bitrate-mode path is unchanged from pre-fix in formula terms (same `est_kbps × replay_seconds × margin / 8`), only the margin changed from 1.5× to 2× — which means the post-fix ceiling is ~80 MB vs pre-fix ~60 MB; both honor the 40 s duration easily. This test confirms no regression in the bitrate path.

---

## Test 4 — US2 / FR-005 slot-start log shows the resolved ceiling

**Validates**: FR-005, SC-004 | **References**: contracts C2 L1

1. Create three slots: a CBR 8 Mbps slot at 1080p30 / 30s replay; a CRF 20 slot at 1080p60 / 30s replay; a CQP 17 slot at 1440p60 / 30s replay.
2. Start each in turn.
3. **Expected**: the plugin log shows one L1 line per slot start, naming (a) the slot, (b) the resolved ceiling in MB, (c) whether the ceiling is auto-derived or user-override-set, (d) the bitrate assumption that drove the auto-derivation.
4. Sum the three reported ceilings; that sum should equal the total memory the host commits to replay buffers (modulo overhead from the encoder pipeline itself). Confirm with Task Manager / Process Explorer's process memory (no exact match expected — replay buffer's lazy allocation means the actual RSS lag-grows up to the ceiling; observe over ~2 minutes).

---

## Test 5 — US2 multi-slot determinism

**Validates**: US2, FR-004, SC-005 | **References**: research D1 (determinism), contracts C1

1. Create two slots with IDENTICAL configurations: same scene-source resolution, same fps, same encoder, same rate control, same replay length. Different slot names.
2. Start both.
3. **Expected**: both slots' L1 lines report the SAME resolved ceiling. Determinism: identical config → identical ceiling.

---

## Test 6 — US3 / FR-011 cap-bound truncation warning fires under cap-bound conditions

**Validates**: US3, FR-011, FR-014, SC-006 | **References**: research D4 + D5 (inference rule), contracts C2 L4 + C3

1. Create slot **TruncTest**: pick a configuration where the auto-derived ceiling is intentionally too small for the actual content bitrate. The easiest way: use the maintainer's CQP-17 reference at 1080p60 (Test 1), but SET the "Max replay buffer size" override to 100 MB (well below the ~653 MB the auto-derivation would produce).
2. Start the slot. Confirm the L1 line reports `100 MB` as the resolved ceiling (user override variant) and notes the auto-derivation would have been ~653 MB.
3. Run high-motion content. Wait ≥ 60 s.
4. Press the save-replay hotkey.
5. **Expected**: TWO log lines for the save —
   - L3: `replay save wrote '<path>' (observed ~30 Mbps, assumed ~68 Mbps)` (the observed bitrate is well above what 100 MB can sustain for 40 s).
   - L4: `replay save likely truncated... observed ~30 Mbps suggests buffer needed ~150 MB but resolved cap is 100 MB (auto-derived assumed ~68 Mbps)...` — the hedged warning, with remediation knobs named.
6. The saved file's actual duration: ~25 s (cap-bound). Confirm in the media player.

---

## Test 7 — FR-006 clamp-and-warn under low host RAM

**Validates**: FR-006, SC-006 | **References**: research D3 (clamp), contracts C2 L2

1. Identify a configuration whose auto-derived ceiling exceeds 50% of the test machine's available physical RAM. Example: on a 16 GB host (≈12 GB free typically), use 4K60 CQP-17 with a 60-second replay. Auto-derived = `0.55 × 3840 × 2160 × 60 × 60 × 2 / 8 / 1024 / 1024 ≈ 3.8 GB`; if 50% of available is, say, 6 GB, this is comfortably below the threshold. To force the clamp, drive the auto-derived value higher: increase replay length to 240 s (4 minutes) → `0.55 × 3840 × 2160 × 60 × 240 × 2 / 8 / 1024 / 1024 ≈ 15 GB`, which exceeds the threshold on any consumer host.
2. Start the slot.
3. **Expected**: L2 (clamp-and-warn) fires. The line names the requested MB, the clamped MB, the available physical RAM, and the four remediation knobs (set override smaller / lower replay duration / lower quality / switch to bitrate-mode).
4. The replay buffer starts at the clamped value (not the requested one); the slot is otherwise operational.

---

## Test 8 — FR-012 override accepted

**Validates**: FR-012, FR-013 | **References**: research D2 (sentinel + persistence), D7 (editor row), contracts C1 (override branch) + C2 L1 user-override variant

1. Create slot **OverrideTest**: any rate-control configuration. Set the "Max replay buffer size" spinbox to 800 (MB).
2. Click Save in the editor. Close the editor.
3. Reopen the editor for the same slot. **Expected**: spinbox shows `800 MB` (the override was persisted via `replay_max_size_mb` in `obs_data_set_int`; reloaded via `obs_data_get_int`).
4. Start the slot.
5. **Expected**: L1 line reports `800 MB` as the resolved ceiling (user override variant), names the auto-derived value alongside for comparison.

---

## Test 9 — FR-012 override below auto-derived (no warning)

**Validates**: FR-013 first paragraph (user-explicit "smaller than auto" choice) | **References**: contracts C1 (override branch overrides auto), C2 (no warning when user explicitly chose smaller)

1. Create a slot whose auto-derived ceiling would be ~653 MB (Test 1's configuration). Set the override to 200 MB (less than auto).
2. Start.
3. **Expected**: L1 line reports `200 MB`. NO warning beyond the L1 line (the user explicitly chose the smaller value; we trust them).
4. Trigger a save. **Expected**: L3 (truthful success). L4 MAY fire if uptime ≥ replay_seconds and the inference indicates cap-bound; that's per the spec FR-013 — the user accepted the trade-off, but the diagnostic still fires so they can decide to raise the override on the next start.

---

## Test 10 — FR-012 backwards compatibility (older save without the field)

**Validates**: FR-012 last paragraph (back-compat), D2 (sentinel = 0 = auto) | **References**: research D2

1. Locate the OBS scene-collection JSON file (typically `%APPDATA%/obs-studio/basic/scenes/<collection>.json`). Find the persisted slot-data for one of the test slots and DELETE the `replay_max_size_mb` key from its JSON (simulating an older save that pre-dates this feature).
2. Restart OBS. Open the slot's editor.
3. **Expected**: the spinbox shows "Auto" (i.e., value 0). The slot loads successfully; no error in the log; the field defaults to auto-derived behavior on start.

---

## Test 11 — FR-014 observed-bitrate suffix in save log

**Validates**: FR-014 | **References**: research D6 (EWMA), contracts C2 L3

1. Start Test 1's slot. Wait at least 30 s (enough samples for the EWMA to settle).
2. Trigger a save.
3. **Expected**: L3 line includes the `(observed ~X Mbps, assumed ~Y Mbps)` suffix. The `observed` figure should be approximately the encoder's actual sustained rate as visible in OBS's main stats dock (cross-check). The `assumed` figure should be the per-second bitrate the auto-derivation used at slot start (~68 Mbps for Test 1's config).
4. Stop the slot. Restart it. Immediately trigger a save (before the EWMA has multiple samples).
5. **Expected**: L3 suffix shows `(observed N/A, assumed ~Y Mbps)` — the EWMA hasn't sampled yet, so `observed` is reported as `N/A` rather than `0` Mbps.
