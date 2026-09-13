#include "../src/SerialPortInfo.h"

#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const wchar_t* name) {
    if (!condition) {
        std::wcerr << L"FAILED: " << name << L'\n';
        ++failures;
    }
}
}

int wmain(int argc, wchar_t** argv) {
    Check(ParseSerialPortName(L"USB-SERIAL CH340 (COM7)") == L"COM7", L"friendly COM7");
    Check(ParseSerialPortName(L"Adapter COM12") == L"COM12", L"COM10+");
    Check(ParseSerialPortName(L"USB-SERIAL CH340").empty(), L"friendly without COM");
    Check(ParseSerialPortName(L"XCOM7Y").empty(), L"COM token boundaries");

    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    ParseUsbVidPid(L"USB\\VID_1A86&PID_7523\\5&123", vid, pid);
    Check(vid == 0x1a86 && pid == 0x7523, L"VID/PID parser");
    ParseUsbVidPid(L"unknown", vid, pid);
    Check(vid == 0 && pid == 0, L"unknown VID/PID");

    const std::vector<SerialPortInfo> sample{{L"COM7"}, {L"COM12"}};
    Check(ContainsSerialPort(sample, L"com7"), L"presence match is case insensitive");
    Check(!ContainsSerialPort(sample, L"COM8"), L"missing port is not present");

    const auto ports = EnumerateSerialPorts();
    for (const auto& port : ports) {
        Check(!port.portName.empty(), L"enumerated PortName");
        Check(ContainsSerialPort(ports, port.portName), L"enumerated port presence lookup");
        std::wcout << L"PORT " << port.portName << L" | " << port.friendlyName
                   << L" | VID=" << std::hex << port.vid << L" PID=" << port.pid
                   << std::dec << L" | " << port.deviceInstanceId << L'\n';
    }
    Check(ContainsSerialPort({SerialPortInfo{L"COM7"}}, L"com7"),
          L"present-port comparison is case insensitive");
    Check(!ContainsSerialPort({SerialPortInfo{L"COM7"}}, L"COM8"),
          L"missing port is rejected");
    if (argc > 1) Check(IsSerialPortPresent(argv[1]), L"requested real port is present");
    if (failures == 0) std::wcout << L"All serial port metadata tests passed.\n";
    return failures == 0 ? 0 : 1;
}
