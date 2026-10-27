# Changelog

本文件记录 evtsys 的重要变更。版本号遵循语义化设计：主版本对应架构/兼容性变更，次版本对应功能，修订号对应缺陷修复。

## [5.0.0] — 2026-10-28

安全性与稳定性修复版本（对应改造路线 v5.0）。保持原有命令行用法与配置文件格式兼容。修复依据见 `evtsys-修改建议.md` 第三节。

### Fixed — P0 缺陷

- **事件订阅失败无法被正确检测**（`winevent.c`）
  原代码用 `WinEventSub == NULL` 判断 `EvtSubscribe()` 的结果——该表达式是数组地址、恒为真，订阅失败永远不会被发现；且 `subscriptions++` 在成功前就执行。现改为接收返回句柄、判断 `NULL` 后再入数组，并加入 `MAX_SUBSCRIPTIONS` 边界保护；失败的订阅不再连带销毁已成功的订阅，渠道名会写入告警日志（原先硬编码 "Unknown"）。
- **释放订阅句柄越界**（`winevent.c`）
  `WinEventCancelSubscribes()` 原以 `while (subscriptions >= 0)` 倒序关闭，首次访问即越界（`WinEventSub[count]`）。现改为 `while (subscriptions > 0)` 先减后取，并对空句柄判空。
- **`realloc()` 返回值丢失**（`winevent.c`、`plugin-login.c`）
  `GetEventInfo()` / `GetLoginInfo()` 调用 `realloc()` 丢弃返回值，一旦堆块移动即产生 use-after-free / 堆损坏。两处重复实现统一为 `WinEventRenderValues()`：接收 `realloc` 返回值、检查 `malloc`/`realloc` 失败路径、校验 `EvtRender` 返回的字段数量，失败时正确释放。
- **网络发送恒报成功**（`wsock.c`）
  `WSockSend()` 无论发送结果均返回 0；错误判断误用 `h_errno`（gethostbyname 语义）而非 `WSAGetLastError()`。现改为：逐目标发送、任一目标成功即返回 0、全部失败返回 1 并计数（`WSockSendFailures`）；使用 `WSAGetLastError()`；TCP 增加 `send()` 短写循环重发（`WSockSendAll`）；连接失败的 socket 重建后重连；`SO_SNDTIMEO` 防止死对端阻塞。发送失败不再导致服务退出（原先恒成功故从未退出；可靠队列见 v5.1 路线）。
- **多个日志服务器初始化被提前中断**（`syslog.c`）
  `SyslogOpen()` 对每个主机以 `if (WSockOpen(...)) return 1;` 串联——首个主机初始化失败即中断，其余主机不再初始化；且连接失败被视为致命错误，服务器不可达会导致服务无法启动。现改为：逐个尝试全部主机、单个失败不中断继续初始化其余目标；连接失败为非致命（保留 socket，由发送路径重连），仅当全部目标都无法打开时才判定启动失败。
- **格式化失败导致事件直接丢弃**（`winevent.c`）
  发布者元数据打开失败或 `EvtFormatMessage()` 取不到消息文本时，原代码直接返回、事件静默丢失。现按三级兜底：① 保留原消息文本；② 回退 `EvtRenderEventXml` 原始事件 XML；③ 回退 Channel / Provider / EventID / RecordID / TimeCreated 标识字段。失败次数计入 `WinEventFormatFailures`，随状态消息输出。

### Fixed — P1 缺陷

- **Windows 服务停止逻辑阻塞**（`service.c`、`loop.c`）
  `ServiceChange()` 原在控制回调里自旋等待 `SERVICE_STOPPED`，可能造成 SCM 停止超时。现改为标准状态机：上报 `SERVICE_STOP_PENDING` → 置停止事件 → 立即返回；`MainLoop()` 用 `WaitForSingleObject(stopEvent, 5000)` 替代固定 `Sleep(5000)`，收到停止信号立即清理并由 `ServiceMain` 上报 `SERVICE_STOPPED`。同时为服务配置失败自动重启（`SERVICE_CONFIG_FAILURE_ACTIONS`）。控制台 Ctrl+C 走同一协作式停止路径，订阅句柄只由管理线程销毁。
- **回调句柄所有权错误**（`winevent.c`）
  `ProcessEvent()` 原对 `EvtSubscribe` 回调传入的事件句柄调用 `EvtClose()`——按 `EVT_SUBSCRIBE_CALLBACK` 契约该句柄由事件服务在回调返回后关闭，自行关闭构成双重释放。已移除全部 `EvtClose(hEvent)`。
- **并发回调静态缓冲区互相覆盖**（`plugin-login.c`、`winevent.c`、`support.c`）
  `pluginLogin()`/`GuidToString()`/`WinEventTimeToString()`/`CollapseExpandMessageW()` 返回共享静态缓冲区，多订阅并发投递时会互相覆盖消息内容。均改为调用方提供缓冲区，回调路径不再有共享可写状态。
- **Unicode 转换缺返回值检查 / 缓冲区不足**（`syslog.c`）
  `MultiByteToWideChar`/`WideCharToMultiByte` 返回值原被忽略；UTF-8 目标缓冲区按 1:1 预留，长中文事件（UTF-8 最多 3 字节/字符）会转换失败静默丢失。现检查返回值、缓冲区扩大为 3 倍，并在转换失败时输出明文占位消息而不是丢弃。

### Fixed — 其他确认缺陷

- **EventID 截断**（`winevent.c`）：按 `EVT_VARIANT` 实际返回的类型读取 EventID（UInt16/UInt32/Int16/Int32），不再固定读 `UInt16Val`。
- **过滤器宽字符缺陷**（`support.c`）：`StartsWith`/`EndsWith` 用 `strlen` 度量宽字符后缀长度，`L"MSOL_"` 被算作 1 个字符，导致所有以 M 开头的登录账户被误过滤。已改用 `wcslen`。
- **内存泄漏**（`winevent.c`）：命中忽略清单提前返回时未释放事件渲染缓冲区，已修复；`WinEventSubscribe` 中每轮泄漏的 `error_msg` 改为一次性分配/释放。
- **隐式函数声明**（`main.h`）：补上 `WIgnoreSyslogEvent` 原型。
- **部分订阅失败误杀服务**（`loop.c`）：个别渠道订阅失败不再终止服务，保留其余订阅继续采集（全部失败才停止）。

### Changed

- 状态消息（`-s` 启用时）追加 `format_failures` / `send_failures` 计数。
- TCP 模式下每条消息附加换行分帧（RFC 6587 non-transparent framing）。
- 事件消息格式化失败时的兜底消息由 Facility/Status 改为 Channel / Provider / EventID / RecordID 标识字段（时间戳已在消息前缀中）。
- 版本号 4.5.1 → 5.0.0（`ver.h`）。

### Noted — 后续版本规划

- v5.1：事件 Bookmark 断点续传、本地持久化队列、断网补发、配置自检。
- v5.2：TLS（RFC 5425）、RFC 5424、IPv6 / 域名解析（`getaddrinfo`）、结构化安全事件、配置热加载、CI 发布。

## [4.5.1] — 历史版本

上游 zensquare/evtsys 基线（fork 时的版本），未维护独立变更记录。历史来源：

- Eventlog to Syslog（Curtis Smith, Purdue University）
- 改造与 Vista/2k8 Windows Events 支持（Sherwin Faria, Rochester Institute of Technology）
- zensquare/evtsys fork
