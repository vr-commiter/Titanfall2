# Titanfall 2 VR · TrueGear

为 Titanfall 2 VR 单人战役提供 TrueGear 触觉反馈。游戏脚本采集动作，游戏进程内的 C++ 插件直接连接 TrueGear Player。

## 玩家使用

需要可正常运行单人战役的 Titanfall 2、CircuitLord Titanfall2VR／Northstar、TrueGear Player 及已连接的设备。玩家无需 Python、Go、.NET、提取器或 `TrueGearSDK.dll`。

1. 完全退出游戏，先备份已有的 `TF2VR/plugins/TrueGear/titanfall2-truegear.json`。
2. 将玩家包 `Titanfall2VR_TrueGear.zip` 解压到游戏根目录，与现有 `TF2VR` 文件夹合并并覆盖。根目录应包含 `Titanfall2.exe`。
3. 需要保留自定义效果映射时，将自己的配置与包内默认配置对照合并，再放回配置路径。
4. 按下一节准备 Player 效果，打开 TrueGear Player 并连接设备，使用原来的 VR 启动方式进入单人战役。七份观察覆盖已在包内，无需运行生成器或应用补丁。

主要文件：

```text
TF2VR/
├─ plugins/
│  ├─ Titanfall2VR_TrueGear.dll
│  └─ TrueGear/
│     ├─ titanfall2-truegear.json
│     ├─ LICENSE
│     └─ licenses/
└─ mods/TrueGear.Events/
   ├─ mod.json
   └─ mod/scripts/vscripts/
```

玩家包包含当前生成并校验的七份观察覆盖。目前适配 CircuitLord VR 1.0.17、Northstar 1.31.13 及源码登记的游戏 DLL／脚本身份。更换 VR、Northstar 或游戏构建后，需要维护者重新核对接口、生成覆盖并打包，不能直接沿用旧覆盖。

本 Mod 不使用发布版本号。配置 `schema`、协议标识和第三方依赖版本用于兼容性检查，需要保留。

## TrueGear Player 效果

**普通事件的 Player 效果资源需要另外准备。** 查看 `TF2VR/plugins/TrueGear/titanfall2-truegear.json` 中启用的 `routes`：`uuid` 是单个效果标识，`hands.left_uuid`／`right_uuid`／`titan_uuid` 分别对应左右手／泰坦。可在 Player 中准备同名效果，或把配置改为已有效果 UUID。名称只允许英文字母、数字、下划线；自定义后以自己的配置为准。配置中的名称本身不包含效果轨道。

`body_slot_effects` 内置十二个身体插槽取放、三个肩部弹药袋成功拿取的 Shake 预设，使用默认 UUID 时无需另建对应 Player 效果。将路线改为预设表以外的 UUID 后，需自行准备对应资源。Player 始终需要运行并连接设备。

死亡使用独立的 `player_death` 效果，需要在 Player 中准备同名基础效果，或修改对应路线 UUID。确认本地玩家死亡后，原生层先清理旧输出，再播放一次死亡效果；持续死亡状态和 CLIENT／SERVER 重复上报不会反复播放，下一次出生后重新准备。

受伤反馈使用两个独立名字：有方向的 `combat_damage_taken` 与无方向的 `combat_damage_taken_nondirectional`。默认 UUID 与事件名相同，需在 Player 分别准备效果，路线可独立配置。摔落、溺水、毒雾／毒液、越界、环境灼烧／电击、烟雾等区域伤害，以及游戏标记不显示方向或泰坦濒毁持续扣血的伤害，使用无方向普通播放。枪击、近战、爆炸及有来源的火焰／电击保留方向播放，缺少有效来源或相机数据时也改用无方向名字。同一次受伤的主事件只播放其中一个；伤害子类型保留原名，并使用相同的方向判定。

## 配置与播放记录

配置启动时读取，修改后重启游戏。`enabled` 控制原生输出，`dry_run=true` 停止实际发送且不产生播放记录。各路线的 `enabled` 控制启用状态，`cooldown_ms` 控制重复反馈间隔。

控制台可用 `tg_events_native 0/1` 暂停／恢复本采集层输出，`tg_events_controller_haptics 0/1` 控制通用左右手柄同步。战斗、移动、泰坦和 VR 分类分别由 `tg_events_combat`、`tg_events_movement`、`tg_events_titan`、`tg_events_vr` 控制。`tg_events_log` 保留兼容，默认 `0`，不再打开脚本事件日志。

本 Mod 仅在原生实际效果播放路径写入精简 `PLAY` 记录，位置为 `TF2VR/plugins/TrueGear/native.log`；不再输出初始化、采样状态、事件 JSON、心跳或错误诊断。`PLAY` 不证明设备已产生穿戴反馈。游戏引擎、VR 和其他 Mod 自己的日志不受影响。

普通效果缺失先核对 Player、UUID 和路线；身体反馈另核对 `body_slot_effects`。自动检查不能代替 Respawn Squirrel 引擎编译及游戏内穿戴实测。

卸载时完全退出游戏，删除 `TF2VR/plugins/Titanfall2VR_TrueGear.dll`、`TF2VR/plugins/TrueGear` 和 `TF2VR/mods/TrueGear.Events`。需要的配置和日志请先另存。插件不支持运行时卸载或热重载。

## 开源源码与构建

`Titanfall2VR_TrueGear-source.zip` 仅保留原生源码、默认配置、事件脚本、CMake 构建文件、实际编译依赖和许可证。测试、诊断、打包程序、本机分析记录、完整第三方参考源码、游戏原稿和七份生成覆盖未收录。保留 `TrueGear.Native` 与 `TF2VR` 的相对位置。独立代码采用 MIT，第三方代码及玩家包中的上游衍生脚本保留其原有授权，见 `PROVENANCE.md` 和随附 notices。

构建需要 Windows x64、Visual Studio 2022 C++ 桌面开发工具及 Windows SDK、CMake 3.24+。原生 DLL 编译无需 Python、Go 或 VPK 提取器；`BUILD_TESTING` 默认关闭。Release 静态链接 MSVC CRT。

在源码解压目录执行：

```bat
cmake -S TrueGear.Native -B TrueGear.Native/build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build TrueGear.Native/build --config Release --parallel
```

事件层保留两个 Python 文件：`tools/build_vr_overrides.py` 实现七份观察 Hook 的插入与覆盖校验，`tools/vpk_sources.py` 实现两份 VPK 来源的读取与身份校验。它们是这部分集成逻辑的源码，不是安装程序；修改或重新生成观察覆盖时才需要 Python 3.10+。

需要重新生成时，在装有兼容游戏／VR／Northstar 的游戏根目录保留相同布局，按 `TF2VR/mods/TrueGear.Events/tools/vendor/SOURCES.md` 获取对应上游源码并构建提取器，放到生成器指定的 `tools/vendor/tf2-vpk2tar.exe`，满足已审阅的 SHA-256 校验后执行：

```bat
python TF2VR/mods/TrueGear.Events/tools/build_vr_overrides.py
python TF2VR/mods/TrueGear.Events/tools/build_vr_overrides.py --check
```

生成器从本机读取 VR／Northstar 原稿并提取两份游戏 VPK 脚本，核对身份、插入点和哈希后生成七份覆盖。开发目录中的 `vr_overrides.manifest.json` 用于维护校验，不是玩家运行依赖，也不放进玩家 ZIP。源码包不附提取器 EXE 或完整 tf2vpk 源码；普通玩家继续使用已经准备好的直接覆盖包即可。
