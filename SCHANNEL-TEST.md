# 国内大厅（Schannel 测试版）

基于 lzusa/SokuLobbiesCN 8b80830（0.7.6.3）的测试构建。

libcurl 仅使用 Windows Schannel 和系统证书库，不链接 mbedTLS，不关闭证书验证。评分传输诊断写入 ratings-debug.log。

与正式大厅只启用一个；关闭游戏后切换。此包是 DLL 测试产物，完整素材与配置由本地安装时从现有正式大厅复制。

SchannelSmoke.exe 使用同一 libcurl 进行 GET 和 POST 实际请求并打印后端及结果。GitHub Windows 测试成功不等于受影响用户的网络问题已解决；SteamOS/Proton 仍需实机验证。
