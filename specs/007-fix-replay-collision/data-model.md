# Phase 1 — Data Model

**Feature**: Replay file uniqueness across slots sharing an output directory + truthful replay-save logging

**Branch**: `007-fix-replay-collision` | **Date**: 2026-05-25

---

## Persisted slot-configuration fields (existing — unchanged in shape and meaning)

Read by the filename-construction and signal-callback code. **No** new fields are introduced by this feature. **No** field is modified in shape or meaning.

| Field | Type | Read by | Notes |
|---|---|---|---|
| `id` | `std::string` | filename helper (`replay_util::build_replay_format`) | Stable opaque persisted slot identifier. Generated once by `generate_slot_id` at `slot.cpp:61-69` (hex `os_gettime_ns()` + hex counter). Last 6 hex chars carry the per-slot uniqueness for the filename (see [research.md](./research.md) D3). |
| `name` | `std::string` | filename helper, `save_replay`'s `request` log line, `on_replay_saved`'s `wrote` log line | User-facing label. Editable. Not enforced unique across slots in a configuration. Embedded in the replay filename after sanitization (see § Sanitization). |
| `path` | `std::string` | OBS replay-buffer `"directory"` setting at `slot.cpp:800` (unchanged) | Per-slot output directory. Two slots MAY point at the same directory (the supported configuration this feature fixes). |
| `container` | `std::string` | OBS replay-buffer `"extension"` setting at `slot.cpp:802` (unchanged) | mp4 / mkv / etc. The replay filename's extension is set by libobs from this field, not from our format string. |
| `replay_enabled` | `bool` | gate at `slot.cpp:785` (unchanged) | Whether the slot configures a replay-buffer output at all. |
| `replay_seconds` | `uint32_t` | OBS replay-buffer `"max_time_sec"` setting at `slot.cpp:803` (unchanged) | Buffer length. |

---

## Runtime "format" setting (changes shape — not persisted)

The plugin passes a `"format"` string to the replay-buffer output via `obs_data_set_string(rb, "format", ...)` at `slot.cpp:801`. This is a **transient libobs setting**, not part of the persisted slot configuration.

### Shape

```
"<NAME>_<ID6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"
```

Where:

- `<NAME>` = `cfg.name` after the sanitization rule below. If sanitization yields an empty string, `<NAME>` is replaced with the literal `"slot"` (matching the existing fallback at `slot.cpp:99` for the continuous recording filename).
- `<ID6>` = the last 6 hex characters of `cfg.id`. Always present. Never empty (the id is generated at slot creation and persisted; it is non-empty for every live slot).
- `Replay_` is a literal that visually disambiguates replay files from continuous recording files in a mixed directory (see [research.md](./research.md) D5).
- `%CCYY-%MM-%DD_%hh-%mm-%ss` is the OBS strftime-style substitution that libobs resolves at file-write time to the wall-clock date-time. Unchanged from the pre-007 format.

OBS resolves the full on-disk path as `<directory>/<format>.<extension>`, where `<directory>` is the `"directory"` setting (the slot's user-configured output path) and `<extension>` is the `"extension"` setting (the slot's container choice). The plugin does NOT construct the full path — only the `<format>` portion.

### Sanitization rule (FR-004a)

`replay_util::sanitize_for_filename(const std::string &name) -> std::string`:

1. For each character in `name`, replace if it is:
   - A Windows-illegal filename character: `<`, `>`, `:`, `"`, `/`, `\`, `|`, `?`, `*`
   - An ASCII control character: any of `\x00` through `\x1F`, or `\x7F` (DEL)
   - The percent sign: `%` (avoids interaction with OBS's strftime substitution parser)

   The replacement character is `_`.

2. Collapse runs of `_` into a single `_`.

3. Strip leading and trailing characters in the set `{_, ., space}`.

4. If the result is non-empty AND its case-folded form matches a Windows reserved device name (`con`, `prn`, `aux`, `nul`, `com1` … `com9`, `lpt1` … `lpt9`), prepend `_`.

5. Return the result.

### Fallback when sanitization yields empty

`replay_util::build_replay_format` substitutes the literal `"slot"` for the empty `<NAME>` component. The full format string becomes `"slot_<ID6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"`. This matches the existing fallback semantics at `slot.cpp:99` for the continuous recording filename.

### Examples

| `cfg.name` | `cfg.id` (last 6 hex) | Resulting format string |
|---|---|---|
| `"CamA"` | `ef1d23` | `"CamA_ef1d23_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` |
| `"Cam A"` | `ef1d24` | `"Cam A_ef1d24_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` *(space is portable)* |
| `"Cam<>A"` | `ef1d25` | `"Cam_A_ef1d25_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` *(<> → _, collapsed)* |
| `""` | `ef1d26` | `"slot_ef1d26_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` |
| `"CON"` | `ef1d27` | `"_CON_ef1d27_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` |
| `"<>"` | `ef1d28` | `"slot_ef1d28_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` *(sanitization empties; fallback kicks in)* |

### Round-trip and migration

- The on-disk slot configuration is **not** touched by this change. `cfg.name`, `cfg.id`, `cfg.path`, `cfg.container` round-trip unchanged.
- Pre-007 replay files on disk (named `Replay_<ts>.<ext>`) are not migrated, renamed, moved, or deleted. They co-exist with post-007 files (`<name>_<id6>_Replay_<ts>.<ext>`) in the same directory. Neither file shape causes scan, load, or save errors.
- A post-007 build opening a pre-007 slot configuration produces post-007 replay filenames immediately on the next save — no opt-in required, no user action needed.
- A pre-007 build opening a post-007 slot configuration produces pre-007 replay filenames (no `<name>_<id6>_` prefix). The bug returns for that build but no data on disk is altered.

---

## Log-line state machine for a single Save Replay operation

A Save Replay triggered against one slot transitions through these states, observable in the plugin log:

```
                                                          [success path]
                                                             │
                  ┌──────────────────────────────────────────│──────────────────────────┐
                  │                                          │                          │
   user trigger   │  save_replay()                           │                          │
   (hotkey/UI/    │  ─ slot_mtx_                             │                          │
    programmatic) │  ─ proc_handler_call("save", &cd)        │                          │
        │         │     returns bool                         │                          │
        │         │                                          │                          │
        ▼         │                                          ▼                          ▼
  ┌─────────┐     │  ┌──────────────────────┐    ┌──────────────────────────┐    ┌────────────────────┐
  │ NONE    ├────►│ │ true → emit          │───►│ replay_buffer_mux_thread │───►│ saved signal fires │
  └─────────┘     │ │  LOG_INFO            │    │ writes file              │    │ → emit             │
                  │ │  '<slot>' replay     │    │  ...                     │    │  LOG_INFO          │
                  │ │  save requested      │    │ on success: muxing=false,│    │  '<slot>' replay   │
                  │ └──────────────────────┘    │  signal "saved"          │    │  save wrote '...'  │
                  │                              │ on error: skip signal    │    └────────────────────┘
                  │ ┌──────────────────────┐    └──────────────────────────┘
                  │ │ false → emit         │                 │
                  │ │  LOG_WARNING         │                 │ [error path]
                  │ │  '<slot>' replay     │                 │ no "saved" signal
                  │ │  save proc-dispatch  │                 │ no plugin log line
                  │ │  FAILED              │                 │
                  │ └──────────────────────┘                 ▼
                  └──────────────────────────────────► [request orphan;
                                                       no follow-up means
                                                       the save failed]
```

### State invariants

1. **One `request` line per `save_replay()` call.** Always emitted (either INFO or WARNING wording).
2. **At most one `wrote` line per `request` line.** The `saved` signal fires exactly once per successful save (per `mux.c:1130-1134`).
3. **A `request` LOG_INFO with no matching `wrote` line within ≤ a few seconds is the user's failure signal.** Causes include: pre-007 cross-slot filename collision (no longer possible after this fix, but the diagnosis path stays valid for *any* future write-failure cause), Windows share-violation race, disk full, output directory missing or read-only, OBS-internal mux error (failed pipe creation at `mux.c:1096`, failed header write at `mux.c:1103`, failed packet write at `mux.c:1110`).
4. **A `request` LOG_WARNING (`proc-dispatch FAILED`) is the proc-handler-level failure signal**, distinct from a mux-level write failure. Typical cause: the slot's replay buffer is not currently capturing (slot not started, or stopped between hotkey press and proc dispatch).

### Threading and synchronization

| Method | Thread | Lock held during execution |
|---|---|---|
| `save_replay()` | UI / hotkey / programmatic caller's thread | `slot_mtx_` (entered at `slot.cpp:1056`) |
| `replay_buffer_mux_thread` | OBS muxer worker (libobs-managed) | None (it's OBS-internal) |
| `on_replay_saved` (signal callback) | OBS muxer worker (same thread that ran the mux) | **None** — no plugin locks. See [research.md](./research.md) D6 and [contracts/replay-save-correctness.md](./contracts/replay-save-correctness.md) § Threading. |
| `log_replay_saved` (instance method called by callback) | OBS muxer worker | **None** |
| `teardown_locked` (during slot stop) | UI thread | `slot_mtx_` (entered by caller `stop()`) |

`signal_handler_disconnect("saved", ...)` in `teardown_locked` is the synchronization barrier: libobs serializes signal dispatch and disconnect on the signal's internal mutex (`.deps/obs-studio-31.1.1/libobs/callback/signal.c`). After the disconnect call returns, the `on_replay_saved` callback is guaranteed not running and cannot fire again. `obs_output_release(replay_out_)` therefore runs after all callbacks have completed; the callback can never read a stale `replay_out_`.

---

## OBS API surface consumed (already in use in this codebase)

| API | Used at | Purpose |
|---|---|---|
| `obs_output_get_signal_handler(replay_out_)` | already used at `slot.cpp:780`, `slot.cpp:821` | Get the output's signal handler for connect/disconnect. |
| `signal_handler_connect(sh, "saved", ...)` | new, in `setup_outputs` next to the existing `"stop"` connect at `slot.cpp:821-822` | Subscribe to mux-completion success signal. |
| `signal_handler_disconnect(sh, "saved", ...)` | new, in `teardown_locked` next to the existing `"stop"` disconnect at `slot.cpp:667-668` | Unsubscribe before output release. |
| `obs_output_get_proc_handler(replay_out_)` | already used at `slot.cpp:1059` for the existing `"save"` call | Get the proc handler for the `get_last_replay` call. |
| `proc_handler_call(ph, "get_last_replay", &cd)` | new, in `log_replay_saved` | Retrieve the on-disk path of the just-saved replay. |
| `calldata_init` / `calldata_string` / `calldata_free` | new, around `proc_handler_call` in `log_replay_saved` | Read the `"path"` out-parameter from the proc call. |
| `obs_data_set_string(rb, "format", ...)` | already used at `slot.cpp:801` | Pass the runtime format string to the replay-buffer output. |

**No new libobs APIs introduced.** Everything used here is already used elsewhere in `slot.cpp`.
