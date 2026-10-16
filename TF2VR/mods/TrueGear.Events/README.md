# TrueGear.Events

Titanfall 2 VR 单人战役事件采集层，与 `Titanfall2VR_TrueGear.dll` 配套使用。事件直接提交到游戏内原生插件；脚本不输出日志。

## 玩家使用

完全退出游戏，备份自定义 `TF2VR/plugins/TrueGear/titanfall2-truegear.json`，将玩家包直接解压到包含 `Titanfall2.exe` 的游戏根目录，与现有 `TF2VR` 文件夹合并并覆盖。需要的自定义效果映射自行合并回配置。

玩家包仅保留运行文件与许可证，已经包含七份生成并校验的观察覆盖。玩家无需应用补丁、运行生成器，也无需 Python 或提取器。当前适配 CircuitLord VR 1.0.17、Northstar 1.31.13 与已核对的游戏文件；更换依赖后需要维护者重新核对接口、生成覆盖并打包。

运行 TrueGear Player 并连接设备。普通抓取、枪械、受伤、移动、泰坦等效果需另外创建或导入，名称以原生配置中启用的 `routes.*.uuid` 及 `hands` 左右手／泰坦映射为准。十五个身体槽／弹药袋 Shake 预设位于 `body_slot_effects`，不需要另建同名 Player 资源。

完整配置、卸载和源码构建说明见源码包内 `TrueGear.Native/README.md`。玩家包不附文档、事件目录、名称清单或开发校验清单。

## 事件与播放记录

源码内的 [EVENTS.md](EVENTS.md) 与 [events.json](events.json) 列出采集入口，不等同于默认效果清单，也不表示每种动作均经实测。`inferred`／`estimated` 表示状态推断或估计。

身体槽位置按世界手位置与身体锚点区分，独立于操作手；世界拾取、换手、副手松开、抛匣和空袋尝试不会当作成功的身体槽取放／拿弹。滑铲沿用 `movement_slide_start`，持续刷新，结束时停止。护盾反馈依据服务器确认的敌方有效命中；涡旋反射要求实际返还发射成功。

受伤先按伤害标记和来源判定能否使用方向。摔落、溺水、毒雾／毒液、越界、环境灼烧／电击、烟雾等区域伤害，以及 `DF_NO_INDICATOR`／`DF_DOOMED_HEALTH_LOSS` 标记的伤害，使用独立的 `combat_damage_taken_nondirectional` 普通播放；即使游戏提供非零坐标，也不据此生成方向。枪击、近战、爆炸与有来源的火焰／电击继续使用 `combat_damage_taken` 方向播放；来源或相机数据不可用时也改用 `combat_damage_taken_nondirectional`。两个默认 UUID 与事件名相同，需在 Player 分别准备效果。同一次受伤的主事件只选其中一个，子类型保留原名并共用方向判定；自伤不单独取消方向。

`TrueGearPlayAngle( name, damageOrigin, fallbackName = "" )` 的可选第三个参数指定缺少方向时的普通播放名；省略时仍退回原名。方向计算成功时使用第一个参数指定的事件。

`player_death` 是独立死亡反馈，需在 Player 准备对应效果。生命周期跟踪实际本地玩家，不跟随死亡镜头中的 NPC 或其他泰坦；死亡先清理已有输出，再播放一次，出生后重新准备。CLIENT 备用死亡观察不受移动分类或 SERVER 移动心跳抑制，原生层合并两侧重复上报。

旧存档缺失的服务端专属入口仍需正常新关卡初始化，CLIENT 备用移动观察不能替代全部服务端事件。通用物品松手、攀爬、滑索、墙跑／挂墙和生命恢复结束没有专用反馈。

`tg_events_enabled` 为采集总开关，`tg_events_native` 控制向原生层提交，默认均为 `1`。`tg_events_log` 仅为兼容保留，默认 `0`，不再启用脚本日志。`tg_events_controller_haptics` 单独控制通用左右手柄同步。

本 Mod 仅在原生实际效果播放路径向 `TF2VR/plugins/TrueGear/native.log` 写入精简 `PLAY` 记录；脚本事件 JSON、初始化、心跳、状态和错误诊断不输出。可选监听器与内部移动心跳仍保留原有分发／去重功能，不写日志。游戏引擎、VR 和其他 Mod 自己的日志不受影响；`PLAY` 记录不证明实际穿戴反馈。

## 源码维护

精简源码包保留 `tg_events_*.nut/.gnut` 和两个集成生成文件：`tools/build_vr_overrides.py` 实现七份观察 Hook 的插入／校验，`tools/vpk_sources.py` 实现两份 VPK 原稿读取／身份校验。这两个文件属于实际集成逻辑，不是安装程序。测试、诊断、打包程序、完整提取器源码、上游原稿和生成覆盖未收录。

普通玩家不运行这些文件。维护者仅在修改或重新生成覆盖时需要 Python 3.10+；先按 `tools/vendor/SOURCES.md` 从对应上游源码构建并放置符合已审阅哈希的提取器，再在装有相同依赖的游戏目录中运行：

```bat
python TF2VR/mods/TrueGear.Events/tools/build_vr_overrides.py
python TF2VR/mods/TrueGear.Events/tools/build_vr_overrides.py --check
```

独立代码采用 MIT。玩家包内的生成覆盖含第三方上游内容，不由本项目 MIT 统一授权；来源和许可说明见配套 `PROVENANCE.md` 及 notices。生成覆盖校验不替代引擎编译与穿戴实测。
