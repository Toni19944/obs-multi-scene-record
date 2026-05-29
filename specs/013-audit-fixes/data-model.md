# Data Model: Apply Codebase Audit Fixes

**Feature**: 013-audit-fixes | **Date**: 2026-05-29

## Overview

This feature introduces no new persistent data entities. The changes are limited to in-memory type changes and new helper structures within the existing C++ class hierarchy.

## Type Changes

### S-002: `slots_` ownership migration

**Before**:
```
SlotManager::slots_ : std::vector<std::unique_ptr<SceneSlot>>
```

**After**:
```
SlotManager::slots_ : std::vector<std::shared_ptr<SceneSlot>>
```

**Impact**: All code that creates, accesses, or iterates `slots_` must use `shared_ptr` semantics. `std::make_unique<SceneSlot>(...)` becomes `std::make_shared<SceneSlot>(...)`. Raw pointer access via `.get()` remains valid for non-owning use.

### S-002: SceneSlot base class addition

**Before**:
```
class SceneSlot { ... };
```

**After**:
```
class SceneSlot : public std::enable_shared_from_this<SceneSlot> { ... };
```

**Constraint**: `SceneSlot` instances MUST be managed by `std::shared_ptr` after this change. Stack allocation or `unique_ptr` ownership becomes undefined behavior for `shared_from_this()`.

## New Structures

### S-002: HotkeyHandle

```
struct HotkeyHandle {
    std::weak_ptr<SceneSlot> wp;
}
```

**Purpose**: Weak-pointer handle passed as `void* data` to OBS hotkey and signal callbacks. Allows callbacks to safely detect slot destruction.

**Lifecycle**: Allocated with `new` in `register_hotkeys()`, freed with `delete` in `unregister_hotkeys()`.

### P-001: SlotSnapshot

```
struct SlotSnapshot {
    std::vector<std::shared_ptr<SceneSlot>> slots;
    size_t generation;
}
```

**Purpose**: Atomic snapshot of all slots under a single mutex acquisition. Used by dock `refresh()` and `refresh_stats()` to eliminate O(N) lock acquisitions per tick.

**Lifecycle**: Stack-allocated, short-lived (duration of a single dock refresh cycle).

### P-005: Slot ID Index

```
SlotManager::id_index_ : std::unordered_map<std::string, size_t>
```

**Purpose**: O(1) lookup of slot index by slot ID string. Rebuilt on `load_from`, `add_slot`, `remove_slot`.

**Invariant**: `id_index_[id] == i` implies `slots_[i]->config().id == id`. Guarded by `SlotManager::mtx_`.

## New Members

### P-004: Cached available memory

```
SlotEditor::cached_avail_mb_ : uint64_t
```

**Purpose**: Caches `available_physical_mb()` result for the dialog's lifetime, eliminating per-keystroke syscalls.

**Lifecycle**: Set once in `SlotEditor` constructor. Read-only thereafter.
