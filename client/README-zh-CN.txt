Lantern-BBSC 客户端

只编辑 config.json：从 config.example.json 复制后，设置服务器地址、端口和注册密钥。
gameRoot 可留空，由启动器寻找或手动选择 Lantern。
accountFile 可留空，首次启动自动创建账号；已有账号可填入原加密账号文件路径。
PLAY、CHECK 和 UDP 检查均读取 config.json，不再读取旧 server-card/settings 文件。
构建和安装步骤见仓库根目录 README.md。

账号凭据在本机 Windows 用户环境下加密，位于 work/accounts，不应上传或互传。
账号文件中的 server 只用于检查账号归属；连接目标以 config.json 为准。
角色进度继续使用 Lantern 原存档路径，没有自动隔离。
画面、手柄和模组设置继续由 Lantern 管理。
更新前完整退出游戏，保留 config.json、work、已有账号文件。
同队所有玩家须使用配套 BBU2 客户端；真实三人和多房间并行联机仍待验证。
