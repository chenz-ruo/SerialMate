# 架构与稳定性说明

## 模块

- `src\main.cpp`：窗口、布局、固定启动默认值、通信记录、文件操作与 UI 状态机。
- `src\SerialPort.*`：串口配置、后台线程、重叠读写和有界发送队列。
- `src\LogWriter.*`：独立结构化日志线程、有界队列和有界退出握手。
- `src\RawRxWriter.*`：独立原始 RX 二进制文件线程；不做编码或结构化格式转换。
- `src\CommRecord.*`：保留原始 `byte[]` 的 `DataRecord` 和独立 `SystemRecord` 模型、HEX/TEXT 格式化、复制与查找。
- `src\CommView.*`：单一原生窗口的可见行绘制、DPI 度量、单垂直/必要时水平滚动和记录级选择。
- `src\TextCodec.*`：集中处理 UTF-8、GBK、严格 ASCII 编码，提供发送编码、历史记录解码和跨接收批次的 `RxTextDecoder`。
- `src\SerialPortInfo.*`：SetupAPI 设备枚举、COM 与设备实例关联、FriendlyName/VID/PID 元数据和下拉框显示名生成。
- `src\UpdateChecker.*`：WinHTTP 异步版本检查和可取消句柄。
- `src\Utilities.*`：HEX、UTF-8、时间戳、错误文本与版本比较。

## 串口线程

串口以 `FILE_FLAG_OVERLAPPED` 打开。后台线程同时等待停止事件、发送队列事件和接收完成事件；所有等待都有退出路径，不在持锁状态执行阻塞 I/O。关闭时先设置停止事件并调用 `CancelIoEx`，再等待线程退出和关闭句柄。

接收数据以 30 ms 或 8 KiB 为批次交给 UI。跨线程 UI 队列最多保留 4 MiB，并只投递一个待处理通知；单次 UI 排空最多处理 128 KiB，即使 UI 暂时被文件对话框占用，也不会无限堆积 Windows 消息。发送队列上限 4 MiB，结构化日志和原始 RX 文件队列各自上限 8 MiB，通信记录模型最多保留 4 MiB 原始字节并按整条记录淘汰旧内容。

通信记录不再依赖 RichEdit 的自动换行。`RecordBuffer` 有界保留最多 4 MiB 原始字节和 16384 条记录；`RecordView` 根据当前客户区、等宽字体度量和 DPI，在 16/12/8/4 字节候选中选择能让 TEXT 列可见的最大行宽，再生成 `VisualRow`，调整窗口只改变显示分行，不改变原始记录。HEX 永远来自 `rawBytes`，TEXT 通过当前 `TextEncoding` 解码，二者不互相反推。`RxTextDecoder` 保存跨 UI/ReadFile 批次的 UTF-8 或 GBK 未完成字节，切换编码时清空 pending 状态。缓存淘汰前缀分别按 16/12/8/4 行宽记账，长时高负载后 Resize 不会因旧的 16 字节基准而跳行。控制或不可解码字节显示为 `.`，CR/LF 不会改变行结构。

`SystemRecord` 不参与 RX/TX、HEX、TEXT 列布局，时间戳和消息从客户区左内边距开始以灰蓝色绘制；关闭时间戳后不保留隐形列宽。分隔线仅在 `DataRecord` 行绘制。暂停时复制、查找和绘制均使用冻结快照，顶部显示“已暂停显示”；底层串口、计数和日志仍继续。用户主动上滚时暂停跟随底部，回到底部或重新启用自动滚动后恢复跟随。日志线程接收原始记录并在后台格式化，避免长报文阻塞串口线程或绘制路径。

## USB 热拔插

设备移除或驱动返回错误时，后台线程停止通信并向 UI 报告。UI 关闭句柄、停止定时发送、恢复参数控件并重新扫描端口。窗口关闭同样走完整取消和回收路径，不依赖设备仍然在线。

## 自动更新

更新线程先请求 Gitee 的 `yycz/SerialMate/raw/main/update-manifest-gitee.txt`，失败后请求 GitHub 的 `chenz12213412/SerialMate/main/update-manifest-github.txt`。清单限制为 4 KiB，包含 HTTPS URL、版本、大小和 SHA-256；下载后再次核对大小和 SHA-256。确认后由临时更新器等待父进程退出，在跨实例更新互斥下备份、替换并启动新 EXE，替换或启动失败会恢复旧版本。网络失败、HTML、损坏清单和取消均静默失败，不影响主程序。

## 无持久化与多开

程序不读取或写入 INI、JSON 或注册表配置，也不保存窗口、串口、编码、发送内容和文件路径状态。每个新进程都从同一组固定默认值启动。自动日志文件名包含毫秒时间和进程 ID，避免不同窗口争用同一文件。

串口下拉框只在启动、展开或设备变更后枚举；SetupAPI 优先从同一设备实例的注册表设备键读取 `PortName`，再读取描述、制造商、硬件 ID、位置和实例 ID，解析 `VID_xxxx/PID_xxxx`，无法识别的 COM 仍以 `COMx` 退化显示。顶部结构化日志与左侧原始 RX 文件是两条独立链路。

