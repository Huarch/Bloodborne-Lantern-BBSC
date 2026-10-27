# Bloodborne-Lantern-BBSC

基于 BBSC 0.0.2 的 Lantern 联机适配源码，包括 Windows 联机模组、Linux 账号/匹配服务、游戏 HTTP 服务与 BBU2 中继。

**实验性项目。** 协议测试和接收通知回归测试通过；最新接收通知修订仍需真实游戏复测。三人游戏、多组房间同时联机未完成实测。仓库不包含 Bloodborne 游戏文件或 Lantern 引擎。

最新修订：全灭重生会固定加载描述中的房主地图和坐标，并验证实际落地地图；死亡动画期间的全灭/房主重生状态持续保留。Boss 战死亡改为在入场前记录的安全位置复活（雾门外附近），未记录到入口时回退到安全灯点，仍需实机验证入口位置。启动桥接保留 Lantern 的作弊选项，在当前进程内适配其在线停用判定，并等待原生作弊校验完成后才安装 BBSC 游戏钩子；引擎文件保持原样。未知原生代码布局会记录日志并跳过该适配。

启动顺序修订：先在游戏镜像附近预留 BBSC 跳板内存，再等待 Lantern 原生作弊校验，最后写入游戏钩子。避免原生初始化占满附近地址空间后出现 `no free memory within reach`、BBSC 钩子全部安装失败。构建脚本包含地址空间占满后的钩子安装回归测试。

## 配置入口

| 部分 | 唯一手动配置文件 | 模板 |
|---|---|---|
| 客户端 | `client/config.json` | `client/config.example.json` |
| 服务端 | `server/config.json` | `server/config.example.json` |

先复制模板再编辑真实配置。地址均为文档占位地址，注册密钥未随仓库提供。两端服务端口需要一致。

客户端 PLAY、CHECK、UDP 检查共用同一配置；选择的游戏路径和账号文件位置也写回该文件。旧 `server-card.json`、`tools/settings.json`、`work/client-settings.json` 不再参与连接配置。迁移时将旧服务器地址、注册密钥和游戏路径填入新配置；保留原账号文件，将 `accountFile` 指向它。不要删除原存档。

服务端由 `python3 server/run.py` 启动；它从单个 JSON 生成私有运行目录中的 shadNet 配置与公告，并为 Python 游戏服务传入参数。生成文件无需手动维护；数据库、加密账号、运行恢复状态属于数据，不是额外配置入口。协议常量及 `worlds.cfg`/`scoreboards.cfg` 是上游静态数据。

## 客户端构建与启动

需要 Windows、Visual Studio x64 C++ 工具链、Windows SDK、LLVM clang-cl。在 **x64 Native Tools Command Prompt** 中将 LLVM 加入 PATH，运行 `build-client.cmd`。

产物位于 `build/bbsc-mod/`，并运行接收通知回归测试。fmt、xbyak 依赖源码已提供。DLL 使用 System V ABI，不应直接换成 MSVC cl 编译。

把 `bbsc.dll`、`bbsc-launch.exe` 放到 `client/bbsc/`；该目录还需 `emulator/mod/mod.json`，根目录的 LICENSE（命名为 LICENSE.txt）、NOTICE.md（命名为 NOTICE.txt）、CREDITS.txt、SUPPORTERS.txt。账号注册程序 `shadnet-sample.exe` 的源码在 `server/shadnet/clientsample/`，按其 CMakeLists 单独构建后放到 `client/account-helper/`。可复用合法取得的同版辅助程序；本仓库只提交源码。

填写 `client/config.json` 后运行 `client/PLAY LANTERN + BBSC.cmd`。首次启动注册独立账号，以后复用 Windows 加密凭据。Lantern 引擎、画面设置和存档路径继续由 Lantern 管理，**存档默认与主游戏共用**。更新前完整退出游戏；同队玩家使用配套版本。

## 服务端构建与启动

需要 Linux、GCC、CMake、Python 3，以及 Qt 6 的 Core、Network、Sql、Concurrent、HttpServer 与 SQLite 插件。参考环境为 Ubuntu 22.04、Qt 6.8.3；服务器无需桌面 UI。Protobuf、Abseil 源码已提供。

```sh
cmake -S server/shadnet -B server/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64 \
  -DFETCHCONTENT_SOURCE_DIR_ABSL="$PWD/server/dependencies/absl"
cmake --build server/build --target shadnet test_multiplayer_relay transparent_relay_probe test_bloodborne_summon_broker -j2
cp server/config.example.json server/config.json
# 编辑 server/config.json，替换文档 IP 和示例密钥。
python3 server/run.py --check
python3 server/run.py
```

配置路径相对配置文件所在目录解析。`paths.qtLibraryPath` 可填 Qt 的 `lib` 目录。密钥可用 `python3 -c "import secrets; print(secrets.token_urlsafe(32))"` 本地生成，保存到忽略的真实配置中，私下提供给受邀玩家。监督进程停止时会停止自己启动的两个服务。

迁移生产环境时先停服备份，将原 `db/` 迁移到配置的 `runtime/shadnet/db/`，原 `online-data.json` 迁移到 `runtime/game/online-data.json`。空运行目录会建立新数据库；不要误当作原有账号丢失。公告统一放在 JSON 的 `notice` 字段。本次仓库配置改造尚未应用到既有生产部署。

默认入站端口：TCP 31313（账号/匹配）、31315（WebAPI）、20443（游戏 API）、20444（BBSC 同步）、18671（可选 bbhost）；UDP 31314（信令/BBU2）、3478（STUN）。`bbhost.relayPorts` 仅供可选 bbhost 客户端使用，BBU2 不使用该范围。出站需允许返回流量。客户端不要求公网 IP 或路由器入站映射。

## 验证

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
./server/build/test_multiplayer_relay
./server/build/transparent_relay_probe
./server/build/test_bloodborne_summon_broker
```

Windows 配置测试：`powershell -NoProfile -ExecutionPolicy Bypass -File tests/test-client-config.ps1`。

## 公开仓库与敏感信息

真实配置、密钥、账号文件、数据库、存档、运行日志、私钥、压缩包和构建产物均不应提交。`.gitignore` 覆盖常见路径，无法保护通过 `git add -f` 或其他名称提交的秘密。发布前检查暂存内容；分享客户端也应从干净目录打包。

## 上游与许可

- [BBSC](https://github.com/CLSAMG22/BBSC)，基础提交 `4f11faf88b6ba658ebde6fe5fbefdadedba4fa41`，包含本地联机补丁。
- [shadnet-p2p](https://github.com/Wozzardman/shadnet-p2p)，基础提交 `5a849a192fd4450593c57b10697316ac324c856e`。
- 角色 ID、召唤队列、Lantern 状态格式、信令地址、BBU2 转发与接收通知补丁在相应源码中。

保留原作者与贡献者署名。主项目许可见 LICENSE、NOTICE.md、CREDITS.txt；服务端及第三方代码遵循 `server/shadnet/LICENSES`、`server/shadnet/REUSE.toml`、`server/gameserver/LICENSE.NoDreamForHunter` 和依赖目录中的许可证。
