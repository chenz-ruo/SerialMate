# SerialMate v1.1.0 测试结果

测试日期：2026-09-13
系统：Windows 11 x64（10.0.26200.0）
编译器：MSVC 19.50，Windows SDK 10.0.26100.0
硬件：USB-SERIAL CH340（COM7，VID 1A86 / PID 7523），TX/RX 短接，115200 8N1

## 发布证据

- 正式候选提交、源码树、构建输入指纹、EXE 大小与 SHA-256 由最终验证生成的 `dist\SerialMate\BUILD_METADATA.txt` 和 `VERIFIED_RELEASE.txt` 记录。
- FileVersion / ProductVersion / CMake / About / manifest 均为 `1.1.0`。
- 产品信息：`SerialMate` / `SerialMate 串口助手` / 作者“如果”。
- 正式 Release 只上传 `SerialMate.exe`；测试程序、PDB、截图和验证文件不作为用户资产上传。

## 自动测试

Release x64 干净构建成功，CTest 12/12 通过：

- Utilities
- UpdateCheckerCancellation
- UpdateInstaller
- CommRecord
- CommView
- TextCodecStream
- SerialPortInfo
- LogWriter
- RxIngressQueue
- SerialPortShutdown
- VersionConsistency
- UiGeometry

关键结果：

- Gitee/GitHub 清单版本不一致时选择更高版本，只有版本号一致时才允许下载 fallback。
- 发布脚本要求 `VerifiedCommit == HEAD`、Tree/指纹/EXE/manifest 一致；已有同版本 Tag 或资产不同时直接拒绝，不使用 `--clobber`。
- Release 主程序使用 MSVC `/Brepro`；相同构建输入的连续 Clean Build 必须生成相同 EXE SHA-256。
- 串口 OVERLAPPED 读、写、`WaitCommEvent` 在事件句柄释放前进入完成、取消或设备断开终态。
- SetupAPI 端口枚举不再为探测占用而打开串口；占用或共享冲突只在用户真正打开时提示。
- Release 测试使用显式失败计数和非零返回值，不依赖 `assert`。
- 设置界面、配置持久化、常用指令、通信记录查找、旧发送文件和无 UI 入口的 Raw RX 保存代码均已删除。

## 真实串口回环

- SetupAPI 正确识别 `USB-SERIAL CH340 (COM7)`、VID 1A86、PID 7523 和设备实例。
- 100 次 Open/Close 循环、pending read 关闭和 pending write 有界关闭通过，无 WorkerDetached。
- 竞争打开被正确拒绝，不影响原连接；4 MiB 发送队列边界生效。
- ASCII + CRLF、`00 01 0A 0D 7F 80 FE FF`、UTF-8 和 GBK 字节流回环通过。
- 1 MiB 持续回环逐字节一致，无丢失、重复或错序。
- 关闭前 1/10/100/1024-byte 尾包全部完整接收。

## EXE UI 实机回归

- 主窗口标题为“SerialMate 串口助手”，About 显示 v1.1.0；启动窗口在当前显示器工作区居中。
- 首次 1536×1024 布局、1920×1080 布局及返回 1536×1024 的几何坐标一致；通信记录与串口设置顶边、数据发送与发送设置底边对齐。
- 发送编辑框与通信记录使用同一 HFONT 和正常字重。
- 默认为 115200/8/1/None/None，时间戳、自动滚动和十六进制显示开启；接收独显、传统古法、十六进制发送、CR、LF 和定时发送关闭，间隔 1000 ms，UTF-8。
- ASCII、UTF-8 中文、HEX、CR/LF/CRLF/无后缀、定时发送、文件加载后发送和错误自动停止通过。
- 实时记录、结构化日志、通信记录导出、六种复制和局部选区复制通过。
- 暂停显示时接收、计数和记录继续；1.25 MiB 通信视图追加压力下保持有界。
- 关闭串口、新建窗口、多实例、正常退出和重启固定默认值通过。
- 启动、使用、退出和重启全过程不生成 INI、JSON 或注册表配置。

## 物理热拔插

`PhysicalHotplug=NotRun`。本轮不将 SetupAPI 枚举、错误路径或历史截图冒充为新的物理热拔插验证。

## 已知限制

- 实机硬件范围为当前 CH340 COM7；FT232、CP210x 及其他驱动组合未做本轮实机覆盖。
- 物理 USB 拔出/插回未在本轮重新执行，不作为本次 v1.1.0 发布阻塞项。
- 33 ms 通信视图定时器在空闲时仍存在；当前 CPU 开销低，按 P3 记录，本轮不重构 UI 刷新架构。
