# Data Model: CQP value coherence across editor, log, and shared-encoder consumer

**Feature**: 006-cqp-mismatch

This change introduces **one new helper** on `SlotManager` and **one in-memory normalization rule** for `SceneSlot::Config`. It introduces **no new persisted fields** and **no save-format changes** in shape — the existing `rate_control` (string) and `rc_value` (uint32) keys continue to round-trip; only their semantics for consumer slots change (the sentinel is the marker).

## Entities

### `SceneSlot::Config` (existing) — semantics for `rate_control` / `rc_value`

| Field | Type | Pre-006 semantics | Post-006 semantics |
|---|---|---|---|
| `rate_control` | `std::string` | Encoder rate-control mode (`"CBR"`, `"CQP"`, `"CRF"`, `"Lossless"`, ...) for every slot — including consumers, where it was stale and unused. | For an owner slot, unchanged: the mode the encoder is built with. For a consumer slot (`!shared_encoder_slot_id.empty()`), the literal sentinel `"<inherited>"` — never read for any operational decision; every read of the effective mode routes through `SlotManager::effective_rate_control`. |
| `rc_value` | `uint32_t` | Bitrate kbps for bitrate-based modes; quality level for quality-based modes; `6000` default. Same staleness story on consumers. | For an owner slot, unchanged. For a consumer slot, normalized to `0` on load and on save. The number is not used; the resolution helper returns the owner's effective value. |
| `shared_encoder_slot_id` | `std::string` | Empty when the slot owns its encoder; otherwise the `id` of the owner slot whose encoder is reused. | Unchanged. |

The sentinel `"<inherited>"` is **never** written as an encoder property (no `obs_data_set_string(s, "rate_control", "<inherited>")` call site is added). It only exists in the `SceneSlot::Config` POD struct between load and save. The encoder-build paths in `slot.cpp` (`apply_encoder_settings`, `SharedEncoder::build`) read from the **owner's** Config — never from a consumer's — and so never see the sentinel.

### `SlotManager::EffectiveRC` (new struct)

```cpp
// On SlotManager (declaration in manager.hpp):
struct EffectiveRC {
    std::string mode;            // e.g. "CBR", "CQP", "Lossless"
    uint32_t    value;           // bitrate kbps or quality level
                                 // (undefined when is_lossless(mode))
    bool        fallback;        // owner's encoder construction fell back to obs_x264/CBR
    std::string owner_slot_name; // empty when c is its own owner
};

EffectiveRC effective_rate_control(const SceneSlot::Config& c) const;
```

This is the **single source of truth** for the rate-control mode and value as they appear at the encoder. It is the only function user-visible read sites consult (see Decision 5 in [research.md](./research.md)).

### `SharedEncoder::encoder_fallback_` (existing, unchanged)

Already set to `true` in `src/slot.cpp:325` when the requested encoder is unavailable and the build path forces `obs_x264 + CBR`. The new `effective_rate_control` reads this flag (briefly under `shared_mtx_`) to surface the fallback values in the editor and the log.

## State transitions

### Load — `SlotManager::load_from` → `slot_from_data` (modified)

The existing back-compat path (`manager.cpp:337-447`) is augmented with three migration steps, applied **after** the existing defaults so an old save loads correctly first.

```text
slot_from_data(d):
  c = read fields from obs_data            // existing
  apply existing back-compat defaults      // existing (e.g. empty rate_control -> "CBR")

  // === NEW: Decision 2 — consumer-side stale-fields normalization ===
  if (!c.shared_encoder_slot_id.empty()) {
      c.rate_control = "<inherited>";
      c.rc_value     = 0;
      return c;   // skip standalone-only steps below
  }

  // === NEW: Decision 4 — mode substitution (FR-015) ===
  if (encoder_modes(c.video_encoder_id) does not contain c.rate_control) {
      blog(WARNING, "...rate-control '%s' not supported by %s; substituted '%s'...");
      c.rate_control = encoder_modes(c.video_encoder_id).front();
  }

  // === NEW: Decision 3 — value clamp (FR-013) ===
  IntRange r = encoder_range(c.video_encoder_id, c.rate_control);
  if (r.found && (c.rc_value < r.min || c.rc_value > r.max)) {
      uint32_t clamped = clamp(c.rc_value, r.min, r.max);
      blog(WARNING, "...rc value %u out of range for %s on %s [%d, %d]; clamped to %u...");
      c.rc_value = clamped;
  }

  return c;
```

`encoder_modes` and `encoder_range` are thin wrappers over `obs_get_encoder_properties` that share their key list with `set_quality_value` (Decision 7). They are pure reads from libobs — no plugin state, no locks. They are called under `mtx_` only because `slot_from_data` itself is called from `load_from` under `mtx_`; this does not change the lock-order story.

### Save — `SlotManager::save_to` → `slot_to_data` (unchanged surface)

The existing function (`manager.cpp:280-335`) writes `c.rate_control.c_str()` and `c.rc_value` directly. For a consumer slot, those values are the sentinel + 0 (normalized at load time or at editor accept time). For an owner slot, they are the same as before this feature. **No change to `slot_to_data` itself** — it just sees an already-normalized Config.

### Editor accept — `SlotEditor::on_accept` (modified)

Existing flow at `ui-slot-editor.cpp:691-758` distinguishes "shared" from "real" encoder selection by inspecting the combo data string `"shared:<id>"`. The new code adds two assignments in the `shared:` branch:

```text
if (venc_data.startsWith("shared:")) {
    cfg_.shared_encoder_slot_id = venc_data.mid(7).toStdString();
    cfg_.video_encoder_id.clear();
    cfg_.rate_control = "<inherited>";   // NEW
    cfg_.rc_value     = 0;               // NEW
}
```

This satisfies Story 2 Acceptance #1 directly — saving a slot through "switch to shared encoder" cannot leave behind the pre-switch standalone values.

### Editor display — `update_shared_encoder_visibility` (modified)

At `ui-slot-editor.cpp:987-1024`, the existing pattern is:

```cpp
const bool is_shared = data.startsWith("shared:");
set_form_row_visible(form_, rc_combo_, !is_shared);       // hides when shared
set_form_row_visible(form_, rc_value_spin_, !is_shared);  // hides when shared
```

Post-006:

```cpp
const bool is_shared = data.startsWith("shared:");
// Rate-control rows stay visible (FR-005). Disable instead of hide.
if (is_shared) {
    auto eff = SlotManager::instance().effective_rate_control(cfg_);
    QString suffix = !eff.owner_slot_name.empty()
        ? QString(" (inherited from %1)").arg(QString::fromStdString(eff.owner_slot_name))
        : QString();
    if (eff.fallback)
        suffix += " [CBR fallback]";

    rc_combo_->clear();
    rc_combo_->addItem(QString::fromStdString(eff.mode), QString::fromStdString(eff.mode));
    rc_combo_->setEnabled(false);
    form_->labelForField(rc_combo_)->setText("Rate control" + suffix);

    if (rc_util::is_lossless(eff.mode)) {
        rc_value_spin_->setSpecialValueText("— (lossless)");
        rc_value_spin_->setRange(0, 0);
    } else {
        rc_value_spin_->setRange((int)eff.value, (int)eff.value);
        rc_value_spin_->setValue((int)eff.value);
    }
    rc_value_spin_->setEnabled(false);
    form_->labelForField(rc_value_spin_)->setText("Value" + suffix);
}
set_form_row_visible(form_, rc_combo_, true);
set_form_row_visible(form_, rc_value_spin_, true);
```

The `is_shared == false` branch restores the standalone editing behaviour exactly as it is today (re-enables and re-labels the widgets). This is two ~12-LOC additions: the `is_shared`-true branch and the `is_shared`-false restore branch.

### Slot start — `SceneSlot::start()` log line (modified)

Existing log emit at `src/slot.cpp:562-566`:

```cpp
blog(LOG_INFO, "[multi-scene-rec] '%s' started (%ux%u@%u, %s/%u, tracks=%s, %s)",
     cfg_.name.c_str(), cfg_.width, cfg_.height, cfg_.fps_num,
     cfg_.rate_control.c_str(), cfg_.rc_value, tracks_str,
     cfg_.replay_only ? "replay-only" : cfg_.replay_enabled ? "rec+replay" : "rec-only");
```

Post-006 (uses the resolution helper):

```cpp
auto eff = SlotManager::instance().effective_rate_control(cfg_);
char rc_buf[96];
if (rc_util::is_lossless(eff.mode))
    std::snprintf(rc_buf, sizeof(rc_buf), "%sLossless",
                  eff.fallback ? "[CBR fallback] " : "");
else
    std::snprintf(rc_buf, sizeof(rc_buf), "%s%s/%u",
                  eff.fallback ? "[CBR fallback] " : "",
                  eff.mode.c_str(), eff.value);

blog(LOG_INFO, "[multi-scene-rec] '%s' started (%ux%u@%u, %s, tracks=%s, %s)%s%s",
     cfg_.name.c_str(), cfg_.width, cfg_.height, cfg_.fps_num,
     rc_buf, tracks_str,
     cfg_.replay_only ? "replay-only" : cfg_.replay_enabled ? "rec+replay" : "rec-only",
     eff.owner_slot_name.empty() ? "" : " inherited from '",
     eff.owner_slot_name.empty() ? "" : eff.owner_slot_name.c_str());
// trailing single quote omitted from the format for brevity — full code adds it.
```

This satisfies FR-001 / FR-002 / FR-003 / FR-008 / FR-014 (the log line is one of the user-visible surfaces enumerated in FR-014).

### Replay-buffer memory estimate — `SceneSlot::setup_outputs` (modified)

Existing read at `src/slot.cpp:748`:

```cpp
uint32_t est_kbps = rc_util::is_bitrate_based(cfg_.rate_control) ? cfg_.rc_value : 12000;
```

Post-006:

```cpp
auto eff = SlotManager::instance().effective_rate_control(cfg_);
uint32_t est_kbps = rc_util::is_bitrate_based(eff.mode) ? eff.value : 12000;
```

This satisfies FR-014 ("...replay-buffer sizing, and any future operational or user-visible surface MUST resolve to the owner's effective values").

## Validation rules (where they apply)

| Rule | Applied at | Implements |
|---|---|---|
| Consumer slot must not carry standalone `rate_control` / `rc_value` for any operational read | Load (`slot_from_data`), Save-prep (`on_accept`), every consumer read site (helper) | FR-002, FR-004, FR-006, FR-014 |
| `rc_value` must be within encoder's range for the chosen mode | Load (`slot_from_data`) | FR-013 |
| `rate_control` must be in the encoder's reported mode list | Load (`slot_from_data`) | FR-015 |
| Editor display, persisted config, encoder, slot start log all agree | Resolution helper as the single source for editor (`update_shared_encoder_visibility`), encoder (`apply_encoder_settings` reads owner Config), log (start log uses helper) | FR-011 |
| Slot start log indicates "Lossless" rather than a number | Slot start log reformat | FR-008 |
| Fallback values are surfaced in editor and log | Resolution helper consults `SharedEncoder::encoder_fallback_` | FR-003, FR-005 |

## Backward / forward compatibility

- **Older saves on disk that include consumer slots with stale standalone values**: handled by the load-time normalization (Decision 2). The next save writes the normalized form. Users do not need to re-open each slot in the editor; the migration is automatic at load.
- **Saves written by post-006 builds, loaded by pre-006 builds**: the on-disk shape is unchanged (`rate_control` is still a string, `rc_value` is still an int). A pre-006 build would read the `"<inherited>"` sentinel as the rate-control mode — and since it is not in any encoder's mode list, the editor would silently pick combo index 0 (the same silent-clobber path FR-015 exists to fix). This is an acceptable downgrade hazard: a user opening a post-006 save on a pre-006 build is rolling back a bug fix, so seeing the bug return is expected.
- **No on-disk version bump or migration tag is added** — consistent with this codebase's pattern (features 001 / 003 / 004 / 005 all avoided format version bumps; only feature 003's `shared_encoder_slot_id` field added a new key, and it was forward-tolerant via empty-string default).

## Threading and lock order

The new `SlotManager::effective_rate_control` takes `mtx_` briefly (to look up the owner Config via the existing `config_by_slot_id` path) and then `shared_mtx_` briefly (to read `encoder_fallback_`). Both are short, bounded reads. **They are taken serially, never nested.** The global lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` is preserved.

All read sites that newly call the helper are already either UI-thread (editor display) or post-acquire-pre-slot_mtx (slot start log) or under slot_mtx_ (replay-buffer estimate). None of them hold a higher-numbered lock when calling the helper, so the helper's brief acquisitions do not invert the global order.

Constitution Principle III (NON-NEGOTIABLE) is preserved by construction.
