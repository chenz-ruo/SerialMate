#include "SerialPortInfo.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <cwctype>

namespace {

std::wstring PropertyString(HDEVINFO devices, SP_DEVINFO_DATA& data, DWORD property) {
    DWORD type = 0, bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(devices, &data, property, &type, nullptr, 0, &bytes);
    if (!bytes || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)) return {};
    std::vector<BYTE> buffer(bytes + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(devices, &data, property, &type, buffer.data(),
                                           static_cast<DWORD>(buffer.size()), nullptr)) return {};
    return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring InstanceId(HDEVINFO devices, SP_DEVINFO_DATA& data) {
    DWORD chars = 0;
    SetupDiGetDeviceInstanceIdW(devices, &data, nullptr, 0, &chars);
    if (!chars) return {};
    std::wstring value(chars, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(devices, &data, value.data(), chars, nullptr)) return {};
    value.resize(wcslen(value.c_str()));
    return value;
}

std::wstring DevicePortName(HDEVINFO devices, SP_DEVINFO_DATA& data) {
    HKEY key = SetupDiOpenDevRegKey(devices, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return {};
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, L"PortName", nullptr, &type, nullptr, &bytes);
    std::wstring value;
    if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
        value.resize(bytes / sizeof(wchar_t));
        status = RegQueryValueExW(key, L"PortName", nullptr, &type,
                                  reinterpret_cast<BYTE*>(value.data()), &bytes);
        if (status == ERROR_SUCCESS) value.resize(wcslen(value.c_str()));
        else value.clear();
    }
    RegCloseKey(key);
    return value;
}

std::uint16_t HexPart(const std::wstring& value, const wchar_t* key) {
    const auto pos = value.find(key);
    if (pos == std::wstring::npos) return 0;
    wchar_t* end = nullptr;
    const unsigned long number = wcstoul(value.c_str() + pos + 4, &end, 16);
    return end == value.c_str() + pos + 4 ? 0 : static_cast<std::uint16_t>(number & 0xffff);
}

bool PortLess(const SerialPortInfo& a, const SerialPortInfo& b) {
    auto number = [](const std::wstring& value) { return value.size() > 3 ? _wtoi(value.c_str() + 3) : 0; };
    return number(a.portName) < number(b.portName);
}

} // namespace

std::wstring ParseSerialPortName(const std::wstring& text) {
    for (std::size_t pos = 0; pos + 3 < text.size(); ++pos) {
        if (towupper(text[pos]) != L'C' || towupper(text[pos + 1]) != L'O' ||
            towupper(text[pos + 2]) != L'M' || !iswdigit(text[pos + 3])) continue;
        std::size_t end = pos + 3;
        while (end < text.size() && iswdigit(text[end])) ++end;
        if (pos > 0 && iswalnum(text[pos - 1])) continue;
        if (end < text.size() && iswalnum(text[end])) continue;
        return L"COM" + text.substr(pos + 3, end - (pos + 3));
    }
    return {};
}

void ParseUsbVidPid(const std::wstring& text, std::uint16_t& vid, std::uint16_t& pid) {
    vid = HexPart(text, L"VID_");
    pid = HexPart(text, L"PID_");
}

std::vector<SerialPortInfo> EnumerateSerialPorts() {
    std::vector<SerialPortInfo> result;
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return result;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInfo(devices, index, &data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        SerialPortInfo info;
        info.friendlyName = PropertyString(devices, data, SPDRP_FRIENDLYNAME);
        info.deviceDesc = PropertyString(devices, data, SPDRP_DEVICEDESC);
        info.manufacturer = PropertyString(devices, data, SPDRP_MFG);
        info.hardwareId = PropertyString(devices, data, SPDRP_HARDWAREID);
        info.locationInfo = PropertyString(devices, data, SPDRP_LOCATION_INFORMATION);
        info.deviceInstanceId = InstanceId(devices, data);
        info.portName = DevicePortName(devices, data);
        if (info.portName.empty()) info.portName = ParseSerialPortName(info.friendlyName);
        if (info.portName.empty()) info.portName = ParseSerialPortName(info.deviceDesc);
        if (info.portName.empty()) continue;
        ParseUsbVidPid(info.hardwareId + L";" + info.deviceInstanceId, info.vid, info.pid);
        result.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(devices);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
            wchar_t name[256]{}, value[256]{};
            DWORD nameLength = static_cast<DWORD>(std::size(name));
            DWORD valueLength = sizeof(value), type = 0;
            const LONG status = RegEnumValueW(key, index, name, &nameLength, nullptr, &type,
                                               reinterpret_cast<BYTE*>(value), &valueLength);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS || type != REG_SZ) continue;
            const std::wstring port(value);
            const auto found = std::find_if(result.begin(), result.end(), [&](const auto& item) {
                return _wcsicmp(item.portName.c_str(), port.c_str()) == 0;
            });
            if (found == result.end()) result.push_back(SerialPortInfo{port, {}, {}, {}, {}, {}, {}, 0, 0});
        }
        RegCloseKey(key);
    }
    std::sort(result.begin(), result.end(), PortLess);
    result.erase(std::unique(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.portName.c_str(), b.portName.c_str()) == 0;
    }), result.end());
    return result;
}

std::wstring SerialPortDisplayName(const SerialPortInfo& info) {
    std::wstring description = info.friendlyName;
    if (description.empty()) description = info.deviceDesc;
    if (description.empty()) description = info.manufacturer;
    if (description.empty()) description = L"未知串口设备";
    const auto paren = description.find(L" (COM");
    if (paren != std::wstring::npos) description.resize(paren);
    return info.portName + L"   " + description;
}

bool ContainsSerialPort(const std::vector<SerialPortInfo>& ports, const std::wstring& portName) {
    return std::any_of(ports.begin(), ports.end(), [&](const SerialPortInfo& info) {
        return _wcsicmp(info.portName.c_str(), portName.c_str()) == 0;
    });
}

bool IsSerialPortPresent(const std::wstring& portName) {
    return !portName.empty() && ContainsSerialPort(EnumerateSerialPorts(), portName);
}
