#include "ProtocolGenerator.h"

#include "ModbusRtuGenerator.h"

namespace protocol {

Result Generate(const Request& request) {
    switch (request.type) {
    case Type::ModbusRtu:
        return modbus::Generate(request);
    }
    Result result;
    result.error = L"不支持的协议类型";
    return result;
}

}  // namespace protocol
