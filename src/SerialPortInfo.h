#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SerialPortInfo {
    std::wstring portName;
    std::wstring friendlyName;
    std::wstring deviceDesc;
    std::wstring manufacturer;
    std::wstring hardwareId;
    std::wstring deviceInstanceId;
    std::wstring locationInfo;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
};

std::vector<SerialPortInfo> EnumerateSerialPorts();
std::wstring SerialPortDisplayName(const SerialPortInfo& info);
std::wstring ParseSerialPortName(const std::wstring& text);
void ParseUsbVidPid(const std::wstring& text, std::uint16_t& vid, std::uint16_t& pid);
bool ContainsSerialPort(const std::vector<SerialPortInfo>& ports, const std::wstring& portName);
bool IsSerialPortPresent(const std::wstring& portName);
