# Building obs-multi-scene-record from source

This repository is self-contained. It includes the full
**obs-plugintemplate** build system — CMake modules, CI workflows, and
build scripts. The `buildspec.json` pins and auto-downloads every
dependency (libobs 31.1.1, obs-frontend-api, Qt6) into a local `.deps/`
folder during configure. You do **not** need to install OBS from source
or the Qt online installer separately.

---

## 1. Tools to install

### Windows

| Tool | Notes |
|------|-------|
| **Visual Studio 2022** or **VS 2022 Build Tools** | "Desktop development with C++" workload. The Build Tools (no full IDE) are enough. |
| **CMake ≥ 3.28** | `winget install Kitware.CMake`. |
| **Git** | `winget install Git.Git`. |

Nothing else. No Qt installer, no OBS source clone, no vcpkg.

### Linux

| Tool | Notes |
|------|-------|
| **GCC ≥ 12** or **Clang ≥ 15** | C++17 support required. |
| **CMake ≥ 3.28** | From your distro or Kitware's APT repo. |
| **Git**, **ninja**, **pkg-config** | `ninja-build` package on Debian/Ubuntu. |
| System libs | `libx11-dev libxcb1-dev libwayland-dev` and related — the build will report what's missing. |

### macOS

| Tool | Notes |
|------|-------|
| **Xcode** (full, not just CLT) | Provides the SDK and `xcodebuild`. |
| **CMake ≥ 3.28** | `brew install cmake`. |
| **Git** | Preinstalled or `brew install git`. |

---

## 2. Clone and build

```bash
git clone https://github.com/Toni19944/obs-multi-scene-record.git
cd obs-multi-scene-record
```

### Windows

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

### Linux

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

### macOS

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

The first `cmake --preset` run downloads all dependencies into `.deps/` —
this takes a few minutes on a fresh clone. Subsequent configures are fast.

### Confirm the configure output

It must contain these lines — if absent, Qt or the frontend API isn't wired:

```
-- Found Qt6: ...
-- Found obs-frontend-api: ...
```

---

## 3. Install

The build output is in `build_x64/RelWithDebInfo/` (Windows),
`build_x86_64/` (Linux), or `build_macos/` (macOS).

### Windows

Two destinations, both required. Run the shell as Administrator (Program Files):

```powershell
copy build_x64\RelWithDebInfo\multi-scene-record.dll ^
     "C:\Program Files\obs-studio\obs-plugins\64bit\"

mkdir "C:\Program Files\obs-studio\data\obs-plugins\multi-scene-record\locale"
copy data\locale\en-US.ini ^
     "C:\Program Files\obs-studio\data\obs-plugins\multi-scene-record\locale\"
```

The folder name under `data\obs-plugins\` must exactly match the DLL base name
(`multi-scene-record` — set by the `name` field in `buildspec.json`).

### Linux

User-local install, no sudo:

```bash
mkdir -p ~/.config/obs-studio/plugins/multi-scene-record/bin/64bit
mkdir -p ~/.config/obs-studio/plugins/multi-scene-record/data/locale
cp build_x86_64/multi-scene-record.so \
   ~/.config/obs-studio/plugins/multi-scene-record/bin/64bit/
cp data/locale/en-US.ini \
   ~/.config/obs-studio/plugins/multi-scene-record/data/locale/
```

### macOS

```bash
cp -R build_macos/RelWithDebInfo/multi-scene-record.plugin \
   ~/Library/Application\ Support/obs-studio/plugins/
```

---

## 4. Verify

Launch OBS. Open the log: **Help → Log Files → View Current Log**. Look for:

```
[multi-scene-rec] loading v1.0.0
```

Then **View → Docks** should list **Multi-Scene Record**. Enable it.

Per-slot hotkeys appear under **Settings → Hotkeys** once at least one slot
exists: `Toggle Recording: <slot>` and `Save Replay: <slot>`.

---

## Repository structure

```
obs-multi-scene-record/
├── src/                    # Plugin source
│   ├── plugin-main.cpp/hpp
│   ├── slot.cpp/hpp
│   ├── manager.cpp/hpp
│   ├── ui-dock.cpp/hpp
│   └── ui-slot-editor.cpp/hpp
├── data/locale/            # Translations
│   └── en-US.ini
├── cmake/                  # obs-plugintemplate CMake modules
├── .github/                # CI workflows and build scripts
├── build-aux/              # Code formatting runners
├── CMakeLists.txt          # Build definition
├── CMakePresets.json       # Platform build presets
├── buildspec.json          # Project metadata and dependency pins
├── .clang-format           # C++ formatting rules
└── .gersemirc              # CMake formatting rules
```

---

## Troubleshooting

**`Cannot open include file: 'QWidget'`**
Wipe the cache and reconfigure:
`rmdir /S /Q build_x64` then `cmake --preset windows-x64`.

**`circlebuf' was declared deprecated` (warnings-as-errors)**
You're on an old plugin source. Current source uses the `deque` API (OBS 31+).
Pull the latest `src/`.

**`module file failed to load` in OBS**
Usually a Qt ABI mismatch. The template pins Qt to the same version OBS bundles,
so this is rare — but if you swapped the Qt in `.deps/`, rebuild clean. Also
confirm the DLL landed in `obs-plugins\64bit\` and the data folder name matches.

**CMake cache holds an old project name after renaming in `buildspec.json`**
Wipe the build dir: `rmdir /S /Q build_x64`, then reconfigure.

**`cmake --preset` fails on dependency download**
Network or proxy issue. Delete `.deps/` and rerun configure to force a fresh
fetch. Corporate networks may block the obs-deps download host.

---

## Rebuilding after source changes

Just edit files in `src/` and rebuild — no reconfigure needed:

```powershell
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Reconfigure (`cmake --preset ...`) is only needed when you change
`CMakeLists.txt`, `buildspec.json`, or add/remove source files.
