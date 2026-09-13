# 构建说明

## 环境

- Windows Visual Studio 2022 或更新版本，安装“使用 C++ 的桌面开发”工作负载。
- Windows 10/11 SDK。
- CMake 3.21 或更新版本。

## Release 构建

在 PowerShell 中进入项目目录后执行：

替换交付 EXE 前请先正常关闭正在运行的串口助手。日志属于用户数据，不要为运行测试而删除；实机回归默认在 `build\Release` 中生成独立测试文件。

```powershell
.\build-release.ps1
```

或手动执行：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

主程序输出为 `build\Release\SerialMate.exe`。脚本同时复制可交付文件到 `dist\SerialMate`。

## 真实串口回环测试

将待测串口的 TX/RX 短接后执行（以 COM7 为例）：

```powershell
.\test-all.ps1 -Port COM7
```

测试会驱动实际主窗口完成打开、ASCII/HEX、CR/LF、定时发送、文件加载后发送、实时记录、启动日志、导出、多开和关闭，并生成 1536×1024 默认/连接截图与 1920×1080 默认截图供视觉核验。

## Gitee 发布令牌（一次设置）

不建议把令牌写入 Git 或脚本参数。首次发布前运行：

```powershell
.\set-gitee-token.ps1
```

脚本会先检查 Git Credential Manager 中是否已有可用于 Gitee API 的个人访问令牌；有则直接导入，没有则只提示输入一次。令牌验证通过后使用当前 Windows 用户 DPAPI 加密保存。以后直接运行：

```powershell
.\publish-release.ps1 -Version 1.1.0
```

发布脚本会自动读取加密令牌；令牌撤销或轮换时重新运行设置脚本即可。

