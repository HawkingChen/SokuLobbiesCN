# 国内大厅（Schannel 测试版）

使用官方 0.7.6.3 发布包中的 SokuLobbiesMod.dll 原文件（SHA-256: 3f7e979bda4c9e21fcf6b720be6eac6bdc45821163e8e0f31f718d3e7e03cb9a），完整保留官方大厅协议、机签标识及 UI。

插件入口为 LobbySchannel.dll。入口校验原文件 SHA-256 后加载正式 DLL，在大厅初始化前将其五个 libcurl easy API 入口重定向到同版本 libcurl 的 Schannel 构建；原文件不被改写。所有通过该 easy API 的请求改用 Schannel。官方 DLL 中仍存在原 mbedTLS 二进制，但替换后的请求不再使用它。

系统证书验证保留；忽略调用者指定的 PEM 证书包，使用 Windows 系统证书。日志 Schannel-transport.log，每份约 256 KiB，仅保留一份 .old。原 ratings-debug.log 的 backend=mbedTLS 文案属于正式 DLL 的固定文本，实际后端以 Schannel-transport.log 为准。

只支持上述精确 SHA-256，其他版本停用。切换前关闭游戏，与正式大厅只启用一个。

GitHub Windows 测试直接调用正式 DLL 被替换的 API，验证 GET、POST 和系统证书路径，并验证修改过的 DLL 被拒绝；这些测试不代表已完成游戏内实测。

公开仓库的协议 13 与官方发布包协议不符，因此不能直接重建公开源码来替代正式服务器的客户端。
