cmake_minimum_required(VERSION 3.21)
project(SerialMate VERSION 1.1.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
configure_file(src/Version.h.in "${GENERATED_DIR}/Version.h" @ONLY)
configure_file(src/resource.rc.in "${GENERATED_DIR}/resource.rc" @ONLY)
configure_file(src/app.manifest.in "${GENERATED_DIR}/app.manifest" @ONLY)

add_executable(SerialMate WIN32
    src/main.cpp
    src/Product.h
    src/SerialPort.cpp
    src/SerialPort.h
    src/LogWriter.cpp
    src/LogWriter.h
    src/Utilities.cpp
    src/Utilities.h
    src/TextCodec.cpp
    src/TextCodec.h
    src/UiGeometry.h
    src/ExtensionControlIds.h
    src/SerialPortInfo.cpp
    src/SerialPortInfo.h
    src/RxIngressQueue.cpp
    src/RxIngressQueue.h
    src/ProtocolGenerator.cpp
    src/ProtocolGenerator.h
    src/ModbusRtuGenerator.cpp
    src/ModbusRtuGenerator.h
    src/CommRecord.cpp
    src/CommRecord.h
    src/CommView.cpp
    src/CommView.h
    src/ConfigStore.cpp
    src/ConfigStore.h
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/UpdateInstaller.cpp
    src/UpdateInstaller.h
    "${GENERATED_DIR}/resource.rc"
    src/resource.h
    assets/SerialMate.ico
)

target_compile_definitions(SerialMate PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_include_directories(SerialMate PRIVATE ${GENERATED_DIR} src)
target_compile_options(SerialMate PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(SerialMate PRIVATE /MANIFEST:NO)
target_link_libraries(SerialMate PRIVATE
    bcrypt comctl32 setupapi winhttp shlwapi shell32 ole32 comdlg32 uxtheme dwmapi
)

if(MSVC)
    set_property(TARGET SerialMate PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(SerialMate PRIVATE $<$<CONFIG:Release>:/O1 /GL>)
    target_link_options(SerialMate PRIVATE $<$<CONFIG:Release>:/Brepro /LTCG /OPT:REF /OPT:ICF>)
endif()

add_executable(SerialMateTests
    tests/UtilitiesTests.cpp
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(SerialMateTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SerialMateTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(SerialMateTests PRIVATE shlwapi)
if(MSVC)
    set_property(TARGET SerialMateTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(SerialLoopbackTests
    tests/SerialLoopbackTests.cpp
    src/SerialPort.cpp
    src/SerialPort.h
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(SerialLoopbackTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SerialLoopbackTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(SerialLoopbackTests PRIVATE shlwapi)
if(MSVC)
    set_property(TARGET SerialLoopbackTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(UiSmokeTests tests/UiSmokeTests.cpp)
target_compile_definitions(UiSmokeTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(UiSmokeTests PRIVATE ${GENERATED_DIR} src)
target_compile_options(UiSmokeTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET UiSmokeTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(CommRecordTests
    tests/CommRecordTests.cpp
    src/CommRecord.cpp
    src/CommRecord.h
    src/TextCodec.cpp
    src/TextCodec.h
)
target_compile_definitions(CommRecordTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(CommRecordTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET CommRecordTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(CommViewTests
    tests/CommViewTests.cpp
    src/CommView.cpp
    src/CommView.h
    src/CommRecord.cpp
    src/CommRecord.h
    src/TextCodec.cpp
    src/TextCodec.h
)
target_compile_definitions(CommViewTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(CommViewTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET CommViewTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(UpdateCheckerTests
    tests/UpdateCheckerTests.cpp
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(UpdateCheckerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(UpdateCheckerTests PRIVATE ${GENERATED_DIR})
target_compile_options(UpdateCheckerTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(UpdateCheckerTests PRIVATE bcrypt winhttp shlwapi)
if(MSVC)
    set_property(TARGET UpdateCheckerTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(TextCodecStreamTests
    tests/TextCodecStreamTests.cpp
    src/TextCodec.cpp
    src/TextCodec.h
)
target_compile_definitions(TextCodecStreamTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(TextCodecStreamTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET TextCodecStreamTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(SerialPortInfoTests
    tests/SerialPortInfoTests.cpp
    src/SerialPortInfo.cpp
    src/SerialPortInfo.h
)
target_compile_definitions(SerialPortInfoTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SerialPortInfoTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(SerialPortInfoTests PRIVATE setupapi)
if(MSVC)
    set_property(TARGET SerialPortInfoTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(LogWriterTests
    tests/LogWriterTests.cpp
    src/LogWriter.cpp
    src/LogWriter.h
    src/CommRecord.cpp
    src/CommRecord.h
    src/TextCodec.cpp
    src/TextCodec.h
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(LogWriterTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(LogWriterTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(LogWriterTests PRIVATE shlwapi)
if(MSVC)
    set_property(TARGET LogWriterTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(UpdateInstallerTests
    tests/UpdateInstallerTests.cpp
    src/UpdateInstaller.cpp
    src/UpdateInstaller.h
    src/UpdateChecker.cpp
    src/UpdateChecker.h
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(UpdateInstallerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(UpdateInstallerTests PRIVATE ${GENERATED_DIR})
target_compile_options(UpdateInstallerTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(UpdateInstallerTests PRIVATE bcrypt shell32 winhttp shlwapi)
if(MSVC)
    set_property(TARGET UpdateInstallerTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(RxIngressQueueTests
    tests/RxIngressQueueTests.cpp
    src/RxIngressQueue.cpp
    src/RxIngressQueue.h
)
target_compile_definitions(RxIngressQueueTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(RxIngressQueueTests PRIVATE src)
target_compile_options(RxIngressQueueTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET RxIngressQueueTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(SerialPortShutdownTests
    tests/SerialPortShutdownTests.cpp
    src/SerialPort.cpp
    src/SerialPort.h
    src/Utilities.cpp
    src/Utilities.h
)
target_compile_definitions(SerialPortShutdownTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SerialPortShutdownTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(SerialPortShutdownTests PRIVATE shlwapi)
if(MSVC)
    set_property(TARGET SerialPortShutdownTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(VersionConsistencyTests tests/VersionConsistencyTests.cpp)
target_compile_definitions(VersionConsistencyTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(VersionConsistencyTests PRIVATE ${GENERATED_DIR})
target_compile_options(VersionConsistencyTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(VersionConsistencyTests PRIVATE version)
add_dependencies(VersionConsistencyTests SerialMate)
if(MSVC)
    set_property(TARGET VersionConsistencyTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(UiGeometryTests tests/UiGeometryTests.cpp)
target_compile_definitions(UiGeometryTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(UiGeometryTests PRIVATE src)
target_compile_options(UiGeometryTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET UiGeometryTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(ResponsiveUiTests tests/ResponsiveUiTests.cpp)
target_compile_definitions(ResponsiveUiTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ResponsiveUiTests PRIVATE src)
target_compile_options(ResponsiveUiTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_dependencies(ResponsiveUiTests SerialMate)
if(MSVC)
    set_property(TARGET ResponsiveUiTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(AdaptiveRecordLayoutTests
    tests/AdaptiveRecordLayoutTests.cpp
    src/CommView.cpp
    src/CommRecord.cpp
    src/TextCodec.cpp
)
target_compile_definitions(AdaptiveRecordLayoutTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(AdaptiveRecordLayoutTests PRIVATE src)
target_compile_options(AdaptiveRecordLayoutTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET AdaptiveRecordLayoutTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(ConfigStoreTests
    tests/ConfigStoreTests.cpp
    src/ConfigStore.cpp
    src/ConfigStore.h
)
target_compile_definitions(ConfigStoreTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ConfigStoreTests PRIVATE src)
target_compile_options(ConfigStoreTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(ConfigStoreTests PRIVATE shell32 ole32)
if(MSVC)
    set_property(TARGET ConfigStoreTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(CustomDataPersistenceTests tests/CustomDataPersistenceTests.cpp)
target_compile_definitions(CustomDataPersistenceTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(CustomDataPersistenceTests PRIVATE src)
target_compile_options(CustomDataPersistenceTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_dependencies(CustomDataPersistenceTests SerialMate)
if(MSVC)
    set_property(TARGET CustomDataPersistenceTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(ProtocolGeneratorTests
    tests/ProtocolGeneratorTests.cpp
    src/ProtocolGenerator.cpp
    src/ProtocolGenerator.h
    src/ModbusRtuGenerator.cpp
    src/ModbusRtuGenerator.h
)
target_compile_definitions(ProtocolGeneratorTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ProtocolGeneratorTests PRIVATE src)
target_compile_options(ProtocolGeneratorTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(MSVC)
    set_property(TARGET ProtocolGeneratorTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(ProtocolUiTests tests/ProtocolUiTests.cpp)
target_compile_definitions(ProtocolUiTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(ProtocolUiTests PRIVATE ${GENERATED_DIR} src)
target_compile_options(ProtocolUiTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
add_dependencies(ProtocolUiTests SerialMate)
if(MSVC)
    set_property(TARGET ProtocolUiTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
add_test(NAME ProtocolGenerator COMMAND ProtocolGeneratorTests)
add_test(NAME ProtocolUi COMMAND ProtocolUiTests $<TARGET_FILE:SerialMate>)
set_tests_properties(ProtocolUi PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
add_test(NAME ConfigStore COMMAND ConfigStoreTests)
add_test(NAME CustomDataPersistence COMMAND CustomDataPersistenceTests $<TARGET_FILE:SerialMate>)
set_tests_properties(CustomDataPersistence PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
add_test(NAME ResponsiveUi COMMAND ResponsiveUiTests $<TARGET_FILE:SerialMate>)
set_tests_properties(ResponsiveUi PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
add_test(NAME AdaptiveRecordLayout COMMAND AdaptiveRecordLayoutTests)
add_test(NAME Utilities COMMAND SerialMateTests)
add_test(NAME UpdateCheckerCancellation COMMAND UpdateCheckerTests)
add_test(NAME UpdateInstaller COMMAND UpdateInstallerTests)
add_test(NAME CommRecord COMMAND CommRecordTests)
add_test(NAME CommView COMMAND CommViewTests)
add_test(NAME TextCodecStream COMMAND TextCodecStreamTests)
add_test(NAME SerialPortInfo COMMAND SerialPortInfoTests)
add_test(NAME LogWriter COMMAND LogWriterTests)
add_test(NAME RxIngressQueue COMMAND RxIngressQueueTests)
add_test(NAME SerialPortShutdown COMMAND SerialPortShutdownTests)
add_test(NAME VersionConsistency COMMAND VersionConsistencyTests)
add_test(NAME UiGeometry COMMAND UiGeometryTests)

install(TARGETS SerialMate RUNTIME DESTINATION .)
