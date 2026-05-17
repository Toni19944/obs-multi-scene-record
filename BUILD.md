# Building obs-multi-scene-record from source

Build uses the **obs-plugintemplate**. The template's `buildspec.json` pins and
auto-downloads every dependency — libobs, obs-frontend-api, ffmpeg, and Qt6 —
into a local `.deps/` folder during configure. You do **not** install an OBS
dev kit or the Qt online installer separately.

---

## 1. Tools to install

### Windows

| Tool | Notes |
|------|-------|
| **Visual Studio 2022** or **VS 2022 Build Tools** | "Desktop development with C++" workload. The Build Tools (no full IDE) are enough. |
| **CMake ≥ 3.28** | `winget install Kitware.CMake`. The template's presets require 3.28+. |
| **Git** | `winget install Git.Git`. Needed to clone the template and for the template's dependency fetch scripts. |

Nothing else. No Qt installer, no OBS source clone, no vcpkg.

### Linux

| Tool | Notes |
|------|-------|
| **GCC ≥ 12** or **Clang ≥ 15** | C++17. |
| **CMake ≥ 3.28** | From your distro or Kitware's APT repo. |
| **Git**, **ninja**, **pkg-config** | `ninja-build` package. |
| System libs | `libx11-dev libxcb1-dev libwayland-dev` and related — the template's dependency script lists them on first run if missing. |

### macOS

| Tool | Notes |
|------|-------|
| **Xcode** (full, not just CLT) | Provides the SDK and `xcodebuild`. |
| **CMake ≥ 3.28** | `brew install cmake`. |
| **Git** | Preinstalled or `brew install git`. |

---

## 2. Get the plugin template

```bash
git clone https://github.com/obsproject/obs-plugintemplate.git obs-multi-scene-record-build
cd obs-multi-scene-record-build
```

The folder name is arbitrary — it becomes your working directory.

---

## 3. Drop in the plugin source

Copy the plugin's `src/` and `data/` over the template's:

```
obs-multi-scene-record-build/
├── src/
│   ├── plugin-main.cpp / plugin-main.hpp
│   ├── slot.cpp / slot.hpp
│   ├── manager.cpp / manager.hpp
│   ├── ui-dock.cpp / ui-dock.hpp
│   └── ui-slot-editor.cpp / ui-slot-editor.hpp
└── data/
    └── locale/
        └── en-US.ini
```

Delete the template's stub C source — it collides with `plugin-main.cpp` at
link time (duplicate `obs_module_load`):

```bash
# Windows
del src\plugin-main.c
# Linux / macOS
rm src/plugin-main.c
```

Leave `src/plugin-support.h` and `src/plugin-support.c.in` alone — they're
template scaffolding compiled into a small static lib the build expects.

---

## 4. Edit `buildspec.json`

Set the `name` field — this becomes the DLL filename, the CMake project name,
and the install data-folder name:

```json
{
  "name": "obs-multi-scene-record",
  "version": "1.0.0",
  "displayName": "Multi-Scene Record",
  ...
}
```

Leave `dependencies` untouched — that block pins the libobs/Qt versions.

---

## 5. Edit `CMakeLists.txt`

Two changes in the template's root `CMakeLists.txt`.

**5a. Enable Qt and the frontend API.** Near the top, flip both options to `ON`:

```cmake
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
```

(Or skip the file edit and pass `-DENABLE_QT=ON -DENABLE_FRONTEND_API=ON` at
configure time — see step 6.)

**5b. List the plugin's source files.** Find the `target_sources` line that
references the old stub:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)
```

Replace it with:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/slot.cpp
    src/slot.hpp
    src/manager.cpp
    src/manager.hpp
    src/ui-dock.cpp
    src/ui-dock.hpp
    src/ui-slot-editor.cpp
    src/ui-slot-editor.hpp
)
```

If your template version is missing the `if(ENABLE_QT)` block entirely, append
this after the `find_package(libobs REQUIRED)` block:

```cmake
find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    Qt6::Widgets Qt6::Core OBS::obs-frontend-api)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    AUTOMOC ON AUTOUIC ON AUTORCC ON)
```

`AUTOMOC` is required — `ui-dock` and `ui-slot-editor` use `Q_OBJECT`.

---

## 6. Configure and build

### Windows

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

If you skipped step 5a, pass the flags on the first configure:

```powershell
cmake --preset windows-x64 -DENABLE_QT=ON -DENABLE_FRONTEND_API=ON
```

The first `cmake --preset` run downloads all dependencies into `.deps/` — this
takes a few minutes. Subsequent configures are fast.

### Linux

```bash
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64
```

### macOS

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

### Confirm the configure output

It must contain these lines — if absent, Qt or the frontend API isn't wired:

```
-- Found Qt6: ...
-- Found obs-frontend-api: ...
```

---

## 7. Install

The build output is in `build_x64/RelWithDebInfo/` (Windows),
`build_x86_64/` (Linux), or `build_macos/` (macOS).

### Windows

Two destinations, both required. Run the shell as Administrator (Program Files):

```powershell
copy build_x64\RelWithDebInfo\obs-multi-scene-record.dll ^
     "C:\Program Files\obs-studio\obs-plugins\64bit\"

mkdir "C:\Program Files\obs-studio\data\obs-plugins\obs-multi-scene-record\locale"
copy data\locale\en-US.ini ^
     "C:\Program Files\obs-studio\data\obs-plugins\obs-multi-scene-record\locale\"
```

The folder name under `data\obs-plugins\` must exactly match the DLL base name.

### Linux

User-local install, no sudo:

```bash
mkdir -p ~/.config/obs-studio/plugins/obs-multi-scene-record/bin/64bit
mkdir -p ~/.config/obs-studio/plugins/obs-multi-scene-record/data/locale
cp build_x86_64/obs-multi-scene-record.so \
   ~/.config/obs-studio/plugins/obs-multi-scene-record/bin/64bit/
cp data/locale/en-US.ini \
   ~/.config/obs-studio/plugins/obs-multi-scene-record/data/locale/
```

### macOS

```bash
cp -R build_macos/RelWithDebInfo/obs-multi-scene-record.plugin \
   ~/Library/Application\ Support/obs-studio/plugins/
```

---

## 8. Verify

Launch OBS. Open the log: **Help → Log Files → View Current Log**. Look for:

```
[multi-scene-rec] loading v1.0.0
```

Then **View → Docks** should list **Multi-Scene Record**. Enable it.

Per-slot hotkeys appear under **Settings → Hotkeys** once at least one slot
exists: `Toggle Recording: <slot>` and `Save Replay: <slot>`.

---

## Troubleshooting

**`Cannot open include file: 'QWidget'`**
`ENABLE_QT` didn't take effect. Wipe the cache and reconfigure with the flag:
`rmdir /S /Q build_x64` then `cmake --preset windows-x64 -DENABLE_QT=ON -DENABLE_FRONTEND_API=ON`.

**`circlebuf' was declared deprecated` (warnings-as-errors)**
You're on an old plugin source. Current source uses the `deque` API (OBS 31+).
Make sure you copied the latest `src/`.

**No `-- Found Qt6` line, but configure succeeds**
Some Qt6 CMake configs are quiet. If the build then fails on a Qt header,
`ENABLE_QT` is off. If the build succeeds, Qt was found — ignore the missing line.

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
