# SerialMate v1.1.1 Modbus RTU Protocol Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在冻结现有主界面 Geometry 的前提下，为“协议数据生成”Card 完整实现 Modbus RTU `03/04/06/10` 请求帧生成、复制和填入指定自定义槽位。

**Architecture:** `ProtocolGenerator` 提供协议无关入口，`ModbusRtuGenerator` 独立完成解析、校验、帧组装和 CRC。Win32 UI 只创建协议 Card 内部控件并调用公共入口；协议控件使用已有 `protocolCard` Rect 进行内部布局，不参与或反向影响 `MainLayoutGeometry`。

**Tech Stack:** C++17、Win32 API、CMake/CTest、MSVC `/W4 /permissive- /utf-8`。

**Spec:** `docs/superpowers/specs/2026-09-14-modbus-rtu-protocol-generator-design.md`

## Global Constraints

- 支持且仅支持 Modbus RTU 功能码 `03`、`04`、`06`、`10`。
- 不修改现有主界面宽度、Card 高度、水平/垂直 Gap、响应式三列规则、自定义数据 16 槽结构和通信记录 8/16/32 bytes 规则。
- 不修改串口发送、接收和关闭核心路径。
- 所有文本文件使用 UTF-8 无 BOM 和 CRLF；中文必须通过乱码特征扫描。
- 所有新行为先写失败测试并确认因缺少该行为而失败，再写最小实现。

---

### Task 1: 独立 Modbus RTU 生成器

**Files:**
- Create: `src/ProtocolGenerator.h`
- Create: `src/ProtocolGenerator.cpp`
- Create: `src/ModbusRtuGenerator.h`
- Create: `src/ModbusRtuGenerator.cpp`
- Create: `tests/ProtocolGeneratorTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `protocol::Result protocol::Generate(const protocol::Request&)`
- Produces: `std::uint16_t protocol::modbus::CalculateCrc(const std::vector<std::uint8_t>&)`
- Produces: normalized `Result::hex`, `Result::frame`, `Result::crcLow`, `Result::crcHigh`, and Chinese `Result::error`

- [ ] **Step 1: Write the failing public-contract tests**

Create `tests/ProtocolGeneratorTests.cpp` around this public API:

```cpp
#include "ModbusRtuGenerator.h"
#include "ProtocolGenerator.h"

protocol::Request Request(protocol::Function function, const wchar_t* address,
                          const wchar_t* quantity = L"", const wchar_t* value = L"",
                          const wchar_t* data = L"") {
    return {protocol::Type::ModbusRtu, function, L"01", address, quantity, value, data};
}

Check(protocol::Generate(Request(protocol::Function::ReadHoldingRegisters,
                                 L"0000", L"0002")).hex ==
      L"01 03 00 00 00 02 C4 0B", "03 frame differs from standard vector");
Check(protocol::Generate(Request(protocol::Function::ReadInputRegisters,
                                 L"0000", L"0001")).hex ==
      L"01 04 00 00 00 01 31 CA", "04 frame differs from standard vector");
Check(protocol::Generate(Request(protocol::Function::WriteSingleRegister,
                                 L"0001", L"", L"0003")).hex ==
      L"01 06 00 01 00 03 98 0B", "06 frame differs from standard vector");
Check(protocol::Generate(Request(protocol::Function::WriteMultipleRegisters,
                                 L"0001", L"0002", L"", L"000A 0102")).hex ==
      L"01 10 00 01 00 02 04 00 0A 01 02 92 30",
      "10 frame differs from standard vector");
```

Also use literal invalid requests to assert failure for slave `00`, `F8`, `GG`; address `10000`; quantities `0000`, `007E` for reads and `007C` for function 10; value `XYZ`; data `000A 010`; and quantity `0002` with one data word. Assert `frame.empty()`, `hex.empty()`, and non-empty `error` for every invalid request.

- [ ] **Step 2: Register and run the missing implementation test**

Add a `ProtocolGeneratorTests` CMake target containing the test and four new production files, with the repository’s standard compile definitions/options, then add:

```cmake
add_test(NAME ProtocolGenerator COMMAND ProtocolGeneratorTests)
```

Run:

```powershell
cmake -S . -B build
cmake --build build --config Release --target ProtocolGeneratorTests --parallel 1
```

Expected: FAIL because `ProtocolGenerator.h` and the generator implementation do not exist.

- [ ] **Step 3: Add the minimal public types and protocol dispatcher**

Define in `ProtocolGenerator.h`:

```cpp
namespace protocol {
enum class Type { ModbusRtu };
enum class Function : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleRegister = 0x06,
    WriteMultipleRegisters = 0x10,
};
struct Request {
    Type type = Type::ModbusRtu;
    Function function = Function::ReadHoldingRegisters;
    std::wstring slave = L"01";
    std::wstring address;
    std::wstring quantity;
    std::wstring value;
    std::wstring data;
};
struct Result {
    std::vector<std::uint8_t> frame;
    std::wstring hex;
    std::wstring error;
    std::uint8_t crcLow = 0;
    std::uint8_t crcHigh = 0;
    explicit operator bool() const { return error.empty() && !frame.empty(); }
};
Result Generate(const Request& request);
}
```

`ProtocolGenerator.cpp` dispatches `Type::ModbusRtu` to `modbus::Generate` and contains no Modbus parsing logic.

- [ ] **Step 4: Implement strict parsing, frame assembly, CRC, and formatting**

`ModbusRtuGenerator.cpp` must:

1. Trim field edges and accept only 1–2 HEX digits for slave and 1–4 HEX digits for 16-bit scalar fields.
2. Enforce slave `01–F7`, read quantity `1–125`, and function 10 quantity `1–123`.
3. For function 10, remove whitespace from data, reject non-HEX, require a positive multiple of four digits, parse each four-digit word big-endian, and match word count to quantity.
4. Build payload bytes in network order.
5. Calculate CRC from `0xFFFF` with polynomial `0xA001`, append low byte then high byte.
6. Format every byte using uppercase two-digit HEX separated by one space.

Expose in `ModbusRtuGenerator.h`:

```cpp
namespace protocol::modbus {
std::uint16_t CalculateCrc(const std::vector<std::uint8_t>& bytes);
Result Generate(const Request& request);
}
```

- [ ] **Step 5: Verify generator GREEN and mutation-sensitive boundaries**

Run:

```powershell
cmake --build build --config Release --target ProtocolGeneratorTests --parallel 1
.\build\Release\ProtocolGeneratorTests.exe
```

Expected: PASS. Confirm the tests would fail if CRC byte order is reversed, a quantity upper bound is increased by one, or function 10 omits its byte count.

- [ ] **Step 6: Commit the independent generator**

```powershell
git add -- CMakeLists.txt src\ProtocolGenerator.h src\ProtocolGenerator.cpp src\ModbusRtuGenerator.h src\ModbusRtuGenerator.cpp tests\ProtocolGeneratorTests.cpp
git commit -m "feat: add Modbus RTU protocol generator"
```

---

### Task 2: Protocol Card controls and dynamic form

**Files:**
- Modify: `src/ExtensionControlIds.h`
- Modify: `src/main.cpp`
- Create: `tests/ProtocolUiTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `protocol::Generate(const protocol::Request&)`
- Produces: pre-created Win32 controls in the fixed `protocolCard`
- Produces: function-dependent labels and Show/Hide behavior

- [ ] **Step 1: Write the failing protocol UI shell test**

Create an isolated-process test following `CustomDataPersistenceTests.cpp`: copy `SerialMate.exe` into a unique temporary directory, launch it there, query `WM_APP + 100` key `42`, and resize the client to the existing full-extension width.

Add stable IDs in the wished-for test contract:

```cpp
namespace protocolui {
inline constexpr int TypeLabel = 700;
inline constexpr int TypeCombo = 701;
inline constexpr int SlaveLabel = 702;
inline constexpr int SlaveEdit = 703;
inline constexpr int FunctionLabel = 704;
inline constexpr int FunctionCombo = 705;
inline constexpr int AddressLabel = 706;
inline constexpr int AddressEdit = 707;
inline constexpr int QuantityLabel = 708;
inline constexpr int QuantityEdit = 709;
inline constexpr int DataLabel = 710;
inline constexpr int DataEdit = 711;
inline constexpr int Generate = 712;
inline constexpr int FillCustom = 713;
inline constexpr int ResultLabel = 714;
inline constexpr int ResultEdit = 715;
inline constexpr int Copy = 716;
inline constexpr int Status = 717;  // 保留的废弃 ID，用于验证旧状态 HWND 不再创建
inline constexpr int FillSlotFirst = 720;
}
```

The test must assert every HWND exists once, `TypeCombo` contains only `Modbus RTU`, the default slave is `01`, and all visible controls lie inside the actual protocol Card. It must switch the function combo and assert:

- `03/04`: `起始地址` and `寄存器数量`, data row hidden;
- `06`: `寄存器地址` and `写入值`, data row hidden;
- `10`: `起始地址`, `寄存器数量`, and write-data row visible.

- [ ] **Step 2: Run the UI test RED**

Register `ProtocolUiTests` with a dependency on `SerialMate` and a 30-second timeout. Run:

```powershell
cmake --build build --config Release --target ProtocolUiTests --parallel 1
ctest --test-dir build -C Release -R "^ProtocolUi$" --output-on-failure
```

Expected: FAIL because protocol controls do not exist.

- [ ] **Step 3: Pre-create controls without changing main Geometry**

Add the IDs above to `ExtensionControlIds.h`. In `Application::Create`, create every protocol child exactly once, apply existing fonts and flat edit styling, populate protocol/function combos, set `ResultEdit` read-only, and hide all protocol controls initially with the extension controls.

Add these private UI-only helpers to `Application`:

```cpp
void LayoutProtocolControls();
void UpdateProtocolForm();
protocol::Function SelectedProtocolFunction() const;
void SetProtocolControlsVisible(bool visible);
```

`LayoutProtocolControls()` must consume only `layout_.protocolCard`, `layout_.protocolTitle`, and current DPI. It must not modify `layout_`, call `CalculateMainLayoutGeometry`, change window minimum size, or change any Card boundary. Use fixed full-column control widths so existing viewport clipping continues to reveal the Card from left to right.

- [ ] **Step 4: Implement compact dynamic rows**

Lay out rows beneath the existing title in this order: protocol type; slave/function; address/quantity-or-value; optional write data; buttons; result label; result/copy. Do not create a separate status row. Use the actual Win32 Combo height for the first two rows and keep every control in one row vertically aligned. For `03/04/06`, use the resulting 6-row compact layout; for `10`, use the 7-row layout with write data. `UpdateProtocolForm()` changes labels and Show/Hide only, then calls `LayoutProtocolControls()`.

In `Application::Layout`, call `SetProtocolControlsVisible(layout_.extensionVisible)` and `LayoutProtocolControls()` after the existing custom-slot placement. Standard Mode hides all protocol controls; partial/full extension modes keep them shown and let the existing client viewport clip them.

- [ ] **Step 5: Verify UI shell GREEN and Geometry unchanged**

Run:

```powershell
cmake --build build --config Release --target ProtocolUiTests ResponsiveUiTests UiGeometryTests --parallel 1
ctest --test-dir build -C Release -R "ProtocolUi|ResponsiveUi|UiGeometry" --output-on-failure
```

Expected: all three tests PASS. Compare `git diff -- src/UiGeometry.h` and require no Task 2 changes.

- [ ] **Step 6: Commit the UI shell**

```powershell
git add -- CMakeLists.txt src\ExtensionControlIds.h src\main.cpp tests\ProtocolUiTests.cpp
git commit -m "feat: add dynamic Modbus protocol form"
```

---

### Task 3: Generate, copy, and fill-custom interactions

**Files:**
- Modify: `src/main.cpp`
- Modify: `tests/ProtocolUiTests.cpp`

**Interfaces:**
- Consumes: protocol Card edit values and `protocol::Generate`
- Produces: result/feedback UI, real clipboard content, and one targeted custom slot update

- [ ] **Step 1: Add failing end-to-end UI interaction tests**

In `ProtocolUiTests.cpp`, use real HWND messages and literal expectations:

1. Set `SlaveEdit=01`, select `03`, set address `0000`, quantity `0002`, click Generate, and assert `ResultEdit == L"01 03 00 00 00 02 C4 0B"` and `ResultLabel == L"生成结果 · 8 bytes · CRC C4 0B"`.
2. Seed the clipboard with `unchanged`, click Copy, and assert clipboard Unicode text equals the exact result and `ID_TX_HEX` becomes checked.
3. Seed all 16 custom edits with distinct sentinel text and the main send edit with `MAIN-UNCHANGED`; record `WM_APP + 100` query keys `1` and `2`; record the `ID_TX_HEX` check state.
4. Restore HEX to unchecked, send `WM_COMMAND` with `protocolui::FillSlotFirst + 4`, and assert custom slot 5 becomes the generated HEX, `ID_TX_HEX` becomes checked, the frame is not re-encoded, other editors follow the existing normal HEX conversion path, and RX/TX counters do not change.
5. Assert `ResultLabel` is `已填入自定义5` after automatic HEX enable.
6. Clear the generated result in a fresh process, seed clipboard, invoke Copy and FillSlot command, and assert clipboard and all custom slots stay unchanged.
7. Set slave to `00`, click Generate, and assert the previous valid result remains while `ResultLabel` reports a non-empty error.

- [ ] **Step 2: Run interaction tests RED**

Run:

```powershell
cmake --build build --config Release --target ProtocolUiTests --parallel 1
ctest --test-dir build -C Release -R "^ProtocolUi$" --output-on-failure
```

Expected: FAIL because Generate/Copy/Fill commands have no implementation.

- [ ] **Step 3: Implement Generate and feedback rendering**

Add `protocol::Result protocolResult_` to `Application` and implement:

```cpp
void GenerateProtocolFrame();
void UpdateProtocolFeedback(const std::wstring& text);
```

Build `protocol::Request` from the current form and call `protocol::Generate`. On success, replace `protocolResult_`, update the read-only result, and set the result title to `生成结果 · N bytes · CRC LL HH`. On failure, keep the prior valid result/result text unchanged and set the title to `生成失败 · <generator error>`.

- [ ] **Step 4: Implement real clipboard copy**

Implement `CopyProtocolResult()` using the existing `PutClipboardText`. Empty result only updates the result-title feedback and must not call `EmptyClipboard`. Successful copy writes only normalized HEX, enables HEX send through the existing toggle path, and updates the same title. Clipboard failure must not change the HEX setting.

- [ ] **Step 5: Implement the 1–16 popup and targeted fill**

Implement `ShowProtocolFillMenu()` with `CreatePopupMenu`, sixteen numbered entries, `TrackPopupMenu(... | TPM_RETURNCMD, ...)`, and command IDs `FillSlotFirst + 0..15`. Handle those IDs in `Application::Command` through:

```cpp
void FillProtocolResultIntoCustomSlot(int slot);
```

The helper checks for a valid result, calls `SetWindowTextW` only for `extension::EditFirst + slot`, and relies on the existing `EN_CHANGE` path to update persistence state. It must not call `SendText` or `SendData`. After marking the target as protocol HEX, it enables `ID_TX_HEX` through the existing toggle path so the generated frame is preserved while other editors receive the normal HEX-mode conversion.

- [ ] **Step 6: Verify UI interactions GREEN**

Run:

```powershell
cmake --build build --config Release --target ProtocolUiTests --parallel 1
ctest --test-dir build -C Release -R "^ProtocolUi$" --output-on-failure
```

Expected: PASS with explicit checks for generation, clipboard, targeted fill, no send, automatic HEX enable, and no generated-frame re-encoding.

- [ ] **Step 7: Commit protocol interactions**

```powershell
git add -- src\main.cpp tests\ProtocolUiTests.cpp
git commit -m "feat: wire Modbus generation into protocol card"
```

---

### Task 4: Full regression, encoding gate, and release build evidence

**Files:**
- Modify only if a failing test exposes a regression directly caused by Tasks 1–3.

**Interfaces:**
- Verifies: generator, fill/copy, frozen Geometry, existing UI behavior, and COM7 serial paths

- [ ] **Step 1: Build the complete Release tree without warnings**

```powershell
cmake --build build --config Release --parallel 1
```

Expected: exit code 0 and no compiler warnings.

- [ ] **Step 2: Run every registered test**

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Expected: all tests pass, including `ProtocolGenerator`, `ProtocolUi`, `ResponsiveUi`, and `UiGeometry`.

- [ ] **Step 3: Run real COM7 UI regression**

```powershell
.\build\Release\UiSmokeTests.exe .\build\Release\SerialMate.exe COM7
```

Expected: real serial open/send/receive/file/custom/timed-send/close/graceful-exit paths all pass.

- [ ] **Step 4: Inspect the generated UI snapshot**

Open `build\Release\ui-default-1920x1080.bmp` and verify the protocol form is readable, remains inside the pre-existing protocol Card, and does not change any surrounding Card boundary or Gap.

- [ ] **Step 5: Enforce text encoding and diff hygiene**

For every changed `.cpp`, `.h`, `.md`, and `CMakeLists.txt`, verify UTF-8 without BOM, zero bare LF line endings, and zero matches for the project’s standard mojibake signature set. Then run:

```powershell
git diff --check
git status --short
```

Expected: no whitespace errors; only intended source/test/plan changes and pre-existing untracked `artifacts` remain.

- [ ] **Step 6: Record final evidence**

Report the exact `HEAD`, supported functions, CRC implementation, protocol/UI test results, existing Geometry regression, real COM regression, and Release build result. Stop without adding other protocols or changing Geometry.
