# SerialMate v1.1.0 测试结果

测试日期：2026-09-13
系统：Windows 11 x64（10.0.26200.0）
编译器：MSVC 19.50，Windows SDK 10.0.26100.0
硬件：USB-SERIAL CH340（COM7，VID 1A86 / PID 7523），TX/RX 短接，115200 8N1

## 发布物

- 已验证产品代码提交：`fd1aa6d100cdfcfbd3548eaeb18ae6d1737cfb5d`。
- 最终交付提交、源码树与构建输入指纹记录在 `dist\SerialMate\BUILD_METADATA.txt` 和 `VERIFIED_RELEASE.txt`。
- 文件：`SerialMate.exe`。
- FileVersion / ProductVersion：`1.1.0`。
- 最终 EXE 的精确大小与 SHA-256 以同目录 `BUILD_METADATA.txt` 和 `VERIFIED_RELEASE.txt` 为准，避免文档提交时间改变构建元数据后留下陈旧值。
- 产品信息：`SerialMate` / `SerialMate 串口助手` / 作者“如果”。
- 依赖仅为 Windows 系统 DLL：bcrypt、COMCTL32、SETUPAPI、WINHTTP、SHLWAPI、SHELL32、ole32、COMDLG32、KERNEL32、USER32、GDI32、ADVAPI32；未增加第三方 DLL。

## 自动测试

Release x64 干净构建成功，CTest 13/13 通过：

- Utilities
- UpdateCheckerCancellation
- UpdateInstaller
- CommRecord
- CommView
- TextCodecStream
- SerialPortInfo
- LogWriter
- RawRxWriter
- RxIngressQueue
- SerialPortShutdown
- VersionConsistency
- UiGeometry

专项覆盖结果：

- 更新清单在 Gitee/GitHub 版本不一致时选择更高版本；只允许同版本清单互补 URL/SHA/size，禁止新版本号与旧版本资产混用。
- 串口最终读请求使用独立 OVERLAPPED 生命周期并在关闭前完成或取消，不提前关闭事件句柄。
- 结构化日志对 100 MiB 病理 UTF-8 分片保持增量写入，仅保留最多 3 bytes 的未完成后缀，没有随会话增长的副本。
- Release 模式的接收入口队列测试使用显式失败计数，不依赖会被 `NDEBUG` 移除的断言。
- 已连接设备消失时仅核对当前 COM 设备，统一进入断开状态；未连接时不增加轮询。
- 左侧接收/发送设置卡片由统一 DPI 几何计算生成；覆盖 1536×1024、1920×1080、1366×768、1280×720 及 100%/125%/150% DPI。
- 静态扫描确认设置窗口、配置读写、配置常量、常用指令控件/ID/处理器均已删除；源码不再调用 INI、JSON 或注册表配置接口。
- UI 冒烟测试在启动、使用、退出和再次启动后均确认未生成当前或旧版配置文件，并验证第二次启动恢复固定默认值。

## COM7 真实回环

- SetupAPI 正确识别 `USB-SERIAL CH340 (COM7)`、VID 1A86、PID 7523 和设备实例。
- 串口关闭压力：100 次循环通过；总循环约 2.244 s，挂起读关闭约 22 ms，挂起写关闭约 1001 ms，接收 5824 bytes，无 WorkerDetached。
- 连续打开/关闭 10 次通过。
- 串口占用冲突正确拒绝，原连接不受影响。
- 4 MiB 发送队列边界正确执行。
- ASCII + CRLF、任意二进制/HEX、UTF-8、GBK 字节流逐字节回环通过。
- 20 次低流量样本平均约 31 ms，最大约 32 ms。
- 1 MiB 持续回环逐字节一致，无丢失、重复或错序。
- 关闭前 1/10/100/1024-byte 尾包全部完整接收。

## EXE UI 实机回归

最终 `SerialMate.exe` 在 COM7 上实际启动并通过：

- 主窗口标题固定为“SerialMate 串口助手”，不含版本或临时状态；关于窗口显示 v1.1.0。
- 顶部设置入口、设置窗口和数据发送区常用指令整块均不存在；新启动和新建窗口的发送框均为空。
- 默认值为首个可用串口、115200/8/1/None/None、时间戳和自动滚动开启、原始接收保存关闭、文本发送、CR 开、LF 和定时发送关、1000 ms、UTF-8。
- ASCII、UTF-8 中文、HEX、仅 CR、仅 LF、CRLF、无后缀发送。
- 定时发送；错误时立即停用定时发送并只提示一次。
- 文件加载与发送、原始 RX 文件、结构化日志和通信记录导出。
- HEX/TEXT 同源显示、中文 TEXT 解码、时间戳整列隐藏后的布局重排。
- 16/12/8 Bytes 自适应与 TEXT 优先可见；正常尺寸无不必要的水平滚动条。
- SystemRecord 左对齐，不复用方向、HEX 或 TEXT 列坐标。
- 暂停显示时后台 RX、统计、日志与缓冲继续；恢复后正常刷新。
- 六种结构化复制、普通选区复制和通信记录查找。
- 1.25 MiB 通信视图追加压力下保持有界。
- 发送按钮、Enter 发送、清空、文件加载/发送、关闭串口、新建窗口、多实例和正常退出。
- 启动、运行、退出和重启全过程不生成 `SerialMate.ini` 或旧配置文件，不恢复上次串口、编码、CR/LF、定时发送、路径或发送内容。

最终截图已人工核对：1536×1024 默认界面、1920×1080 默认界面和 1536×1024 COM7 连接界面均无设置按钮和常用指令，发送框默认空白；右侧通信记录/数据发送卡片与左侧三张卡片上下边界协调，状态栏完整，连接截图中 HEX/TEXT 同行且中文可见。

## 物理热拔插

`PhysicalHotplug=NotRun`。

本轮没有要求用户再次进行真实拔出/插回，因此没有把端口枚举、模拟测试或历史截图写成新的物理热拔插通过。该项按任务约定不是 v1.1.0 Release 阻塞项。

## 已知限制

- 本轮实机硬件范围为当前 CH340 COM7；FT232、CP210x 和其他驱动组合未做实机覆盖。
- 单次发送/发送队列和通信记录采用明确的有界策略，长时间采集建议启用日志或原始 RX 保存。
- Gitee 仓库重命名和线上发布需要当前 Windows 用户先完成一次 DPAPI 令牌保存；此项不影响本地 EXE 验收结论。
