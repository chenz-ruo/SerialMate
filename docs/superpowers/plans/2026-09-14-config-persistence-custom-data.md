# SerialMate v1.1.1 Config Persistence and Custom Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist 16 hidden custom-data slots safely and send each slot through the same TX pipeline as the main editor.

**Architecture:** A focused `ConfigStore` owns UTF-8 INI parsing, stable path selection, named-mutex merging, atomic replacement, and hidden attributes. `Application` owns UI state and delegates persistence to `ConfigStore`; all text sources call one send builder and one queue/record path.

**Tech Stack:** C++17, Win32 file/process/window APIs, CMake, MSVC, existing SerialMate test executables.

**Spec:** `docs/superpowers/specs/2026-09-14-config-persistence-custom-data-design.md`

## Global Constraints

- Product version remains v1.1.1.
- `MaximumStoredCustomSlots` remains 16 and each value is limited to 4096 `wchar_t`.
- Configuration is UTF-8 without BOM, CRLF, named `SerialMate.ini`, and has `FILE_ATTRIBUTE_HIDDEN` added without dropping existing attributes.
- Existing UI Geometry, Card gaps, progressive reveal, and 8/16/32-byte rules are frozen.
- Modbus RTU is outside this plan.
- Every production change follows a failing-test-first cycle.

---

### Task 1: ConfigStore loading and stable path selection

**Files:**
- Create: `src/ConfigStore.h`
- Create: `src/ConfigStore.cpp`
- Create: `tests/ConfigStoreTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `config::ConfigData`, `config::ConfigStore::Open(primary, fallback)`, `InitialData()`, `Path()`, and `UsingFallback()`.
- Consumes: Win32 filesystem APIs and `std::filesystem::path`.

- [ ] **Step 1: Write failing loading tests**

Create real temporary directories and assert that `Open()` returns 16 empty values when no file exists; parses `ConfigVersion=1`; preserves Chinese, spaces, HEX and additional `=` characters; ignores unknown keys; and defaults only malformed, invalid UTF-8, or over-4096 fields.

```cpp
auto store = config::ConfigStore::Open(primary, fallback);
Check(store.InitialData().customData[0] == L"中文 = 01 03");
Check(store.InitialData().customData[15].empty());
```

- [ ] **Step 2: Run the new target and verify RED**

Run: `cmake --build build --config Release --target ConfigStoreTests --parallel 1`

Expected: compilation fails because `ConfigStore.h` and its API do not exist.

- [ ] **Step 3: Implement minimal parsing and path selection**

Define:

```cpp
namespace config {
inline constexpr int kConfigVersion = 1;
inline constexpr std::size_t kSlotCount = 16;
inline constexpr std::size_t kMaximumCustomDataLength = 4096;
struct ConfigData { std::array<std::wstring, kSlotCount> customData{}; };
class ConfigStore {
public:
    static ConfigStore Open(const std::filesystem::path& primary,
                            const std::filesystem::path& fallback);
    const ConfigData& InitialData() const;
    const std::filesystem::path& Path() const;
    bool UsingFallback() const;
};
}
```

Use strict per-value UTF-8 decoding, split on the first `=`, preserve the remainder exactly, and make the LocalAppData file authoritative for fields it contains when Primary exists but cannot be written.

- [ ] **Step 4: Verify GREEN**

Run `ConfigStoreTests`; expect all loading and path-selection cases to pass.

### Task 2: Atomic save, hidden attribute, and multi-instance merge

**Files:**
- Modify: `src/ConfigStore.h`
- Modify: `src/ConfigStore.cpp`
- Modify: `tests/ConfigStoreTests.cpp`

**Interfaces:**
- Produces: `bool SaveMerged(const ConfigData&, const std::bitset<16>& dirty, std::wstring& error)`.
- Consumes: the active path fixed by Task 1.

- [ ] **Step 1: Write failing persistence tests**

Test initial file creation, CRLF and no BOM, all 16 keys, 4096-character round trip, Hidden plus retained Archive attribute, absence of stale PID temp files, original-file preservation on failed replacement, and two independently opened stores merging different dirty slots.

```cpp
std::bitset<16> firstDirty; firstDirty.set(0);
std::bitset<16> secondDirty; secondDirty.set(15);
Check(first.SaveMerged(firstData, firstDirty, error));
Check(second.SaveMerged(secondData, secondDirty, error));
Check(config::ConfigStore::Open(primary, fallback).InitialData().customData[0] == L"A");
Check(config::ConfigStore::Open(primary, fallback).InitialData().customData[15] == L"B");
```

- [ ] **Step 2: Run tests and verify RED**

Expected: compilation fails because `SaveMerged` is absent.

- [ ] **Step 3: Implement locked atomic save**

Inside a path-derived `Local\\SerialMate.Config.<hash>` mutex, reread the current file, overlay only dirty slots, write `SerialMate.ini.tmp.<PID>` with `CreateFileW`/`WriteFile`, call `FlushFileBuffers`, close, replace with `ReplaceFileW` or `MoveFileExW`, and reapply saved attributes OR `FILE_ATTRIBUTE_HIDDEN`. Remove only this process's temp file on failure.

- [ ] **Step 4: Verify GREEN**

Run `ConfigStoreTests`; expect persistence, merge, atomicity, and attributes to pass.

### Task 3: Application persistence integration

**Files:**
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/ResponsiveUiTests.cpp`

**Interfaces:**
- Consumes: `ConfigStore::Open`, `InitialData`, and `SaveMerged`.
- Produces: 16 restored edit controls, per-slot dirty tracking, and one normal-exit save.

- [ ] **Step 1: Write failing native UI persistence tests**

Copy the built EXE to an isolated directory, launch it, set slot 1, 8, and 16 through `WM_COPYDATA`, close normally, relaunch, reveal the extension, and assert all three texts are restored while the default client width and Geometry query values remain unchanged.

- [ ] **Step 2: Run and verify RED**

Expected: restarted controls are empty and no hidden `SerialMate.ini` exists.

- [ ] **Step 3: Wire ConfigStore into Application**

Add `std::unique_ptr<config::ConfigStore> configStore_`, `ConfigData customData_`, `std::bitset<16> customDataDirty_`, and `loadingConfig_`. Load after all 16 controls are created, handle custom-edit `EN_CHANGE`, suppress dirty marking during restore, and save once from `Shutdown()`. Keep `EM_SETLIMITTEXT` at 4096.

- [ ] **Step 4: Verify GREEN and Geometry freeze**

Run `ResponsiveUiTests`, `UiGeometryTests`, and `AdaptiveRecordLayoutTests`; expect persistence checks and all existing coordinates to pass.

### Task 4: Unified TX builder and direct custom sending

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/UiSmokeTests.cpp`

**Interfaces:**
- Produces: `BuildSendDataFromText(const std::wstring&, std::vector<std::uint8_t>&, bool, int)` and `SendText(const std::wstring&, bool, int)`.
- Consumes: current HEX, encoding, CR/LF, timer, serial queue, record, counter, and logger state.

- [ ] **Step 1: Write failing behavior tests**

Use test messages to set a custom slot and trigger its button. Assert the main editor text is unchanged, manual custom send cancels active timed send, valid sources produce the same queued bytes, and malformed HEX/ASCII input does not increase TX count.

- [ ] **Step 2: Run and verify RED**

Expected: the old custom handler temporarily changes the main editor and lacks source-specific validation reporting.

- [ ] **Step 3: Implement the common path**

Pass source text into the existing builder, retain one encode/parse/CR/LF implementation, and route both main and custom sources through one serial-send/counter/record function. Use custom slot index only to create the error title `自定义数据 N 格式错误`; do not mutate custom or main text.

- [ ] **Step 4: Verify GREEN**

Run native UI tests and non-hardware send-construction/error tests; expect direct custom sending and unchanged main text.

### Task 5: Real loopback, restart coverage, and release verification

**Files:**
- Modify: `tests/UiSmokeTests.cpp`
- Modify: `CMakeLists.txt` only if a separate persistence UI target is required.

**Interfaces:**
- Consumes: the completed application and existing virtual-COM test harness.
- Produces: end-to-end evidence for persistence and TX equivalence.

- [ ] **Step 1: Add end-to-end cases**

Compare main-editor and custom-slot loopback bytes for ASCII, UTF-8 Chinese, GBK Chinese, HEX, and CR/LF. Add restart verification for CustomData01, CustomData08, and CustomData16 using the isolated copied EXE.

- [ ] **Step 2: Run targeted tests**

Run `ConfigStoreTests`, `ResponsiveUiTests`, `UiGeometryTests`, and `AdaptiveRecordLayoutTests`. Run the existing UI smoke/COM loopback command with the configured virtual port and report unavailable hardware explicitly rather than fabricating success.

- [ ] **Step 3: Run full verification**

Run `cmake --build build --config Release --parallel 1`, then `ctest --test-dir build -C Release --output-on-failure`. Record `CommRecord` and `CommView` timing without changing thresholds.

- [ ] **Step 4: Validate artifacts**

Copy `build\Release\SerialMate.exe` to `dist\SerialMate\SerialMate.exe`, compare SHA-256 hashes, scan every modified text file for mojibake, UTF-8 BOM, and LF-only newlines, and run `git diff --check`.

