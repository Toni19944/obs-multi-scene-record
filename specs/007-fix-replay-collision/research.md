# Phase 0 — Research

**Feature**: Replay file uniqueness across slots sharing an output directory + truthful replay-save logging

**Branch**: `007-fix-replay-collision` | **Date**: 2026-05-25

Eight design decisions resolved. No NEEDS CLARIFICATION items remain.

---

## D1 — OBS replay-buffer outcome-confirmation API surface

**Decision**: Subscribe to the `"saved"` signal on the replay-buffer output's signal handler. When it fires, call the output's `"get_last_replay"` proc handler to retrieve the on-disk path of the just-written file.

**Rationale**: Inspection of `.deps/obs-studio-31.1.1/plugins/obs-ffmpeg/obs-ffmpeg-mux.c` confirms both APIs are part of the replay-buffer output's public surface:

- `obs-ffmpeg-mux.c:964`: `signal_handler_add(sh, "void saved()");` — added during `replay_buffer_create`.
- `obs-ffmpeg-mux.c:1130-1134`: the signal is emitted from `replay_buffer_mux_thread` **only on the success path** (`if (!error)`):
  ```c
  if (!error) {
      calldata_t cd = {0};
      signal_handler_t *sh = obs_output_get_signal_handler(stream->output);
      signal_handler_signal(sh, "saved", &cd);
  }
  ```
  On the error path (failed pipe creation, header write failure, packet write failure — all at `mux.c:1097-1113`), the signal is **NOT** emitted. This gives the plugin a clean success / non-success signal: signal fired ⇒ file is on disk; signal not fired ⇒ no file (or not yet, but the mux thread has either completed-with-error or hasn't completed).
- `obs-ffmpeg-mux.c:961`: `proc_handler_add(ph, "void get_last_replay(out string path)", get_last_replay, stream);` — added during `replay_buffer_create`.
- `obs-ffmpeg-mux.c:943-948`: `get_last_replay` returns `stream->path.array` (the fully-substituted on-disk path) via the calldata's `"path"` out-parameter, but **only when `muxing == false`** (i.e., the save is complete). Calling it from inside the `saved`-signal callback satisfies this precondition by construction (the mux thread has set `os_atomic_set_bool(&stream->muxing, false)` at `mux.c:1128`, right before emitting the signal).

The `"saved"` signal has empty calldata — the path is **not** passed in the signal payload, only via the separate `get_last_replay` proc. The plan accordingly calls `get_last_replay` from inside the callback.

**Alternatives considered**:

- *Hook the muxer's `"file_changed"` signal* (`mux.c:748`): this signal is emitted by the **recording** muxer when output paths change (used for OBS's split-recording feature), not by the replay-buffer muxer. Wrong API surface.
- *Read `obs_output_get_last_error` after `proc_handler_call("save")` returns*: `last_error` reflects the **output**'s last error (encoder failure, output-can't-start, etc.), not a per-save mux failure. The replay-buffer's save is dispatched asynchronously into the mux thread; `last_error` is not set when the mux thread fails to write the file. Wrong abstraction.
- *Verify file existence at the expected path after a delay*: requires predicting the path (the `%CCYY` etc. substitution is done inside libobs, so the plugin would need to duplicate the format-substitution logic). Fragile; locale-dependent; not the OBS-blessed channel.
- *Add a timer that warns if no `saved` signal fires within N seconds*: extra state, extra thread, marginal benefit. A `request` log line with no follow-up `wrote` line is already the user's explicit failure signal (per FR-011's "neutral wording when outcome unknown" path). Defer.

---

## D2 — Choice of identifier source

**Decision**: Use **both** the sanitized `cfg_.name` AND a prefix derived from `cfg_.id`, in that order, in every replay filename. Neither alone is sufficient.

**Rationale**: Settled in the clarification session (Session 2026-05-25, Q2). The chosen filename shape is `<sanitized-name>_<id6>_Replay_<ts>.<ext>` where:

- `<sanitized-name>` (from `cfg_.name`) carries the human-readable attribution — the user can tell which slot produced which file by reading the filename, matching the convention used at `slot.cpp:96-104` for the continuous recording filename. Satisfies US2 / FR-003 (a).
- `<id6>` (from `cfg_.id`) carries the structural uniqueness guarantee — two slots in the same configuration are guaranteed to have distinct ids (by construction in `generate_slot_id()` at `slot.cpp:61-69`), so their filenames are guaranteed distinct without any runtime conflict-detection logic. Satisfies FR-004.

The combination is robust under all the edge cases the spec calls out: empty name (id6 alone identifies), same name across two slots (id6 differs), name containing only sanitized-away characters (degenerates to id6 alone), filesystem case-insensitivity (id6 differs regardless of case).

**Alternatives considered** (all rejected during clarification, recorded here for traceability):

- *Name-only with conditional id-suffix on collision*: requires runtime conflict detection that's both racy (concurrent saves can both pass the "no conflict" check before either writes) and easy to skip (the implementation might forget the check on a future code path). Rejected in Q2.
- *Id-only*: collision-proof but breaks readable attribution from filename alone. Rejected in Q2 (it would violate US2 / FR-003 / SC-002).
- *Name-only with a uniqueness enforcement on slot names elsewhere*: introduces a new system constraint (unique slot names) outside this feature's scope. Rejected in Q2.

---

## D3 — Identifier length and extraction window

**Decision**: Use the **last 6 hex characters** of `cfg_.id` as the `<id6>` component.

**Rationale**: `generate_slot_id()` at `slot.cpp:61-69` produces an id with the shape `"%llx%x"` — a hex-encoded `os_gettime_ns()` timestamp followed by a hex-encoded `static atomic<uint32_t>` counter. Typical length: ~17-19 chars.

- The **first** N characters of the id are the high bits of `os_gettime_ns()`. Two slots created within the same OS-tick (which can happen during a multi-slot scene-collection load) share these high bits — picking the front of the id leaves them indistinguishable.
- The **last** N characters of the id cover (a) the counter (which is incremented per-creation and guaranteed unique within the process lifetime) and (b) the low bits of ns (which differ across rapid successive calls). Picking the tail is structurally safe for the rapid-creation case.

Length: 6 hex chars = 24 bits = ~16.7 million distinct values. For any plausible configuration (dozens to low hundreds of slots), the birthday-paradox collision probability is negligible. The choice trades a 7-character filename overhead (`_<id6>` ≈ 7 chars) for unambiguous uniqueness with zero runtime conflict-detection logic.

**Alternatives considered**:

- *First 6 chars of id*: rejected — sees only the ns-timestamp high bits, fails for slots created within the same OS-tick.
- *Full id (~17-19 chars)*: structurally safest but ~20 chars of filename overhead per save. The user sees a noisier directory listing. Marginally less readable. Last-6 is the right cost/safety trade-off.
- *32-bit FNV-1a hash of the full id (8 hex chars)*: cleaner-distributed than truncation, but adds a 10-line hash function for a problem already solved by truncating the tail. The current id format already places the high-entropy bits at the tail; rehashing buys nothing.
- *Modify `generate_slot_id` to produce a fixed-length id*: out of scope for this feature; would touch the persisted on-disk id field; pre-007 saves would not match. Rejected.

**Forward compatibility**: if `generate_slot_id` is ever changed in the future, the "last 6 chars of `cfg_.id`" rule continues to be a stable, content-agnostic truncation. As long as the new id format puts high-entropy bits in the tail, the rule keeps working. If a future id format puts entropy at the head, the rule would need re-evaluation — but that's a coupled change, not a silent regression.

---

## D4 — Sanitization character set policy

**Decision**: Sanitize `cfg_.name` for filename embedding by replacing the following characters with `_`, then collapse runs of `_`, then strip leading/trailing `_`, `.`, and space:

- Windows-illegal filename characters: `<`, `>`, `:`, `"`, `/`, `\`, `|`, `?`, `*`
- ASCII control characters: `\x00` through `\x1F`, plus `\x7F` (DEL)
- Percent sign `%` (to prevent collision with OBS's strftime-style format-string parser at the `"format"` setting)

Additionally, if the sanitized name (case-insensitive) matches a Windows reserved device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`), prepend `_` to disambiguate.

After all of the above, if the sanitized name is empty, the filename's name component is substituted with the literal `"slot"` (matching the existing fallback at `slot.cpp:99` for the continuous recording filename), and the `<id6>` component carries identity. The fallback `"slot"` literal is intentionally the same word used by the recording filename — it produces visual parity (both pipelines use the same fallback term).

**Rationale**:

- *Windows-illegal set* is the most restrictive of the supported platforms. macOS (`/` and `:`) and Linux (`/`) are strictly subsets. Sanitizing against the Windows set produces a name portable everywhere.
- *Control characters* are universally unsafe on filesystems and cause display oddities in file managers. No legitimate slot name needs them.
- *Percent sign* is the OBS format-string substitution prefix (`%CCYY`, `%MM`, etc.) at `os_generate_formatted_filename`. A name containing a literal `%` could trigger unintended substitution at the format-string-evaluation point inside libobs. Stripping `%` from the sanitized name component is a structural safeguard, not a contract claim on every OBS substitution rule in every locale.
- *Reserved device names* — `CON.mp4`, `NUL.mkv`, etc. fail to create on Windows even though the characters themselves are legal. The case-insensitive prepend-`_` rule (`CON` → `_CON`) is the standard remediation.
- *Empty-after-sanitization fallback*: rather than emitting a leading `_id6_` (which would look like a typo'd filename), substitute the same `"slot"` literal the recording filename already uses. The result is `slot_<id6>_Replay_<ts>.<ext>` — visually consistent with the recording pipeline.

**Alternatives considered**:

- *POSIX-only sanitization (`/` and `\x00` only)*: insufficient for Windows. Rejected.
- *URL-percent-encoding for illegal chars*: produces filenames like `%3CCamA%3E_...` which are harder to read than `_CamA__...`. Bad UX trade.
- *Allow `%` and rely on OBS substitution rules*: documented as a plan-phase assumption in the spec (Assumptions line on strftime tolerance), but accepting `%` would couple the plugin to OBS-substitution invariants we don't control. Strip-and-be-safe is the right default.
- *Apply sanitization to `cfg_.name` at editor save-time (persist sanitized)*: would change the slot's stored name; user types `CamA<>` and the editor stores `CamA__` — visible behaviour change. Spec FR-008 explicitly forbids altering persisted slot settings. Rejected.

---

## D5 — Filename component order

**Decision**: `<sanitized-name>_<id6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss` — slot identity (name then id6) first, then the literal `"Replay_"` marker, then the timestamp.

**Rationale**:

- Per FR-005, the slot identity component must appear in the same position relative to the timestamp as the continuous recording filename. The recording filename at `slot.cpp:96-104` is `<name>_<ts>.<ext>` — name first. Replay follows the same convention.
- The `"Replay_"` literal stays in the format. It visually disambiguates replays from continuous recordings in a mixed directory (`CamA_<ts>.mkv` is a recording, `CamA_<id6>_Replay_<ts>.mkv` is a replay). Removing `"Replay_"` would make the two file types share the same filename shape modulo the `_<id6>_` segment — too fragile.
- Identity-first sorting groups all of a slot's saves together when the user views the directory sorted alphabetically — replay clips and continuous recording from the same slot land adjacent in the file manager.

**Alternatives considered**:

- *`Replay_<name>_<id6>_<ts>.<ext>`* (literal first, identity second): the directory then sorts all replays from all slots together, instead of all files from one slot together. Less useful for the multi-slot case.
- *Drop `"Replay_"`*: rejected — see above.

---

## D6 — Lifetime and locking model for the `saved` signal callback

**Decision**: The `on_replay_saved` callback takes **no plugin locks**. It reads `cfg_.name` and `replay_out_` lock-free, matching the existing pattern in `on_replay_output_stop` (`slot.cpp:1028-1042`). The synchronization barrier is `signal_handler_disconnect("saved", ...)` in `teardown_locked` — libobs serializes signal dispatch and disconnect on the signal's internal mutex (see `.deps/obs-studio-31.1.1/libobs/callback/signal.c`), so by the time `signal_handler_disconnect` returns, the callback is guaranteed not running.

**Rationale**:

- `teardown_locked` is called from `stop()` which holds `slot_mtx_`. If the callback took `slot_mtx_`, libobs's disconnect-waits-for-dispatch would deadlock (callback waiting for `slot_mtx_` held by the disconnecting thread, disconnect waiting for the callback to finish before returning).
- The two fields the callback reads (`cfg_.name` and `replay_out_`) are safe to read lock-free under the existing project convention. `cfg_.name` can be modified by the editor while the slot runs; reading-while-writing a `std::string` is a tolerated mild race in this codebase (the existing `on_replay_output_stop` does the same at `slot.cpp:1037-1038`). `replay_out_` is set during `setup_outputs` and not modified again until `teardown_locked`'s release step, which runs after the disconnect; the callback can never see a torn or stale `replay_out_` because libobs's dispatch-disconnect serialization guarantees the callback returns before the disconnect returns and therefore before the release runs.

**Threading invariant** (captured in [contracts/replay-save-correctness.md](./contracts/replay-save-correctness.md)):

1. `setup_outputs` (UI thread, under `slot_mtx_`): create `replay_out_`, then `signal_handler_connect("saved", ...)`. No save can be in flight yet.
2. `save_replay` (hotkey thread, under `slot_mtx_`): emit `request` log line, dispatch `save` proc. Returns quickly; the actual mux runs on the OBS mux thread.
3. `replay_buffer_mux_thread` (OBS worker thread): writes the file, sets `muxing = false`, emits `saved` signal.
4. `on_replay_saved` (OBS worker thread, NO plugin locks): emit `wrote` log line with the `get_last_replay` path.
5. `teardown_locked` (UI thread, under `slot_mtx_`): `signal_handler_disconnect("saved", ...)` blocks until in-flight dispatch completes, then `obs_output_stop` + `wait_for_output_stop` + `obs_output_release`. After disconnect returns, no more callbacks can fire.

**Alternatives considered**:

- *Callback takes `slot_mtx_`*: deadlocks with disconnect-from-teardown. Rejected.
- *Callback uses a try-lock and skips logging if it can't acquire*: introduces non-determinism in the log (some saves get a `wrote` line, some don't, depending on race timing). Rejected — undermines FR-012 / SC-007.
- *Callback uses a separate dedicated mutex just for log emission*: extra state for a one-line log emission, no real benefit. Rejected.
- *Queue the log emission onto the UI thread via `QMetaObject::invokeMethod` (matching the pattern at `slot.cpp:1007`)*: would work but adds a thread hop for a synchronous log line. The current pattern (lock-free log from the worker thread) is what `on_replay_output_stop` uses; consistency is the better choice.

---

## D7 — Wording of the "neutral when outcome unknown" log line

**Decision**: Replace the current `'<slot>' replay save OK` / `'<slot>' replay save FAILED` (at `slot.cpp:1066`) with:

- **LOG_INFO** `'<slot>' replay save requested` — when `proc_handler_call(ph, "save", &cd)` returned `true`. Neutral wording; does not imply a file was written.
- **LOG_WARNING** `'<slot>' replay save proc-dispatch FAILED (slot not capturing?)` — when the proc call returned `false`. The current `FAILED` wording is preserved here because this *is* a failure that the plugin can directly observe (proc dispatch returning false means the save signal was rejected — typically because the replay buffer isn't active for this slot).

The `wrote` line emitted from the `saved` callback (D8) is:

- **LOG_INFO** `'<slot>' replay save wrote '<path>'` — emitted only when the `saved` signal fires, so the file is guaranteed on disk and `<path>` comes from `get_last_replay`.

**Rationale**:

- *"save requested"* matches the user's suggested wording in the clarification context ("downgrade the log to 'save requested' to avoid implying success"). Standard usage; not a domain-of-art coinage.
- Keeping `FAILED` for the proc-dispatch-failed case preserves the existing signal value: today's `FAILED` is reliable (the proc returning false is a real failure); the bug is in the OK case, not the FAILED case.
- Pairing `requested` with the eventual `wrote` line gives the user a clean two-line audit trail per successful save and a one-line orphan (the `request` with no `wrote` follow-up) for any failed save. The user can grep for "requested" and "wrote" with matching slot names to find mismatches.

**Alternatives considered**:

- *"save dispatched"*: same idea, less natural in English. Rejected.
- *Demote the `request` line to LOG_DEBUG*: would hide the user-action audit trail at default log verbosity. Rejected — the existing line is LOG_INFO and the user relies on seeing it; the change is wording, not level.
- *Suppress the `request` line entirely and only log the `wrote` line on success*: failure cases produce zero log output then, leaving the user with no signal that their hotkey was pressed. Rejected.

---

## D8 — Where to emit the truthful follow-up "wrote" log line

**Decision**: Emit from the `on_replay_saved` callback (the signal handler), as `'<slot>' replay save wrote '<path>'`. One log line per successful save. The path is obtained by calling the replay-buffer output's `"get_last_replay"` proc from inside the callback.

**Rationale**:

- The `saved` signal fires from `replay_buffer_mux_thread` only on the success path (D1) and after `muxing = false` is set, satisfying `get_last_replay`'s precondition at `mux.c:946`.
- The path returned by `get_last_replay` is the actual on-disk path the muxer wrote to (`stream->path.array` at `mux.c:947`), with all `%CCYY` etc. substitutions resolved. This is the exact path the OBS muxer's own `[ffmpeg muxer: 'replay_out_<id>'] Wrote replay buffer to '<path>'` log line (at `mux.c:1118`) reports — by construction the plugin's `wrote` line agrees with OBS's (FR-013).
- Emitting from the callback (rather than polling or post-hoc) keeps the latency between "file is on disk" and "user sees the path in the log" tight (within a single signal dispatch), which matters when a user is troubleshooting a multi-slot save spree and needs to know which clip landed where.

**Alternatives considered**:

- *Emit nothing extra; rely on the OBS muxer's own `Wrote replay buffer to` log line*: would satisfy the truthfulness requirement (FR-011) but leaves FR-012 (path in plugin's own log) unmet — the user would need to cross-reference two log lines from different subsystems to confirm which slot wrote where. The plugin's log line includes the slot name; the OBS line does not, only the internal output id. Rejected.
- *Emit from a UI-thread queued callback (Qt invokeMethod) rather than the worker thread*: adds latency and a thread hop for a single `blog` call. The existing project pattern (`on_replay_output_stop`) emits from the worker thread directly. Match the pattern.
- *Emit a richer line (file size, duration, encoder)*: scope creep; the spec asks for the path, which is sufficient for the user's verification task (FR-012 / SC-007).
