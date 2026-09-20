# TARibbon V1.3 验证清单（中文）

本轮（2026-09-12）仅实施源码并做静态检查。未运行自动化测试、UBT/UHT、ShaderCompileWorker、Cook、Editor、PIE、GPU Capture 或视觉/性能验收。“静态核对完成”只表示以下代码路径已核对，不代表运行效果通过。

## A. 静态接口核对

| 项目 | 应核对内容 | 状态 |
| --- | --- | --- |
| Collider ABI | `UTARibbonColliderComponent : UPrimitiveComponent` 独立存在；无 Mesh/材质 authored 字段；Chaos participation disabled | 静态核对完成 |
| Shape defaults | Sphere `R=50 cm`；Capsule `R=50 cm/H=100 cm`（含半球、local +Z）；Plane 原点/local +Z 单侧；preview halfextent `100 cm` 仅线框 | 静态核对完成 |
| Scale/diagnostic | 仅正均匀缩放；无效变换进入诊断并排除物理快照；绿色/灰色/红色线框语义一致 | 静态核对完成；画面待验收 |
| Contacts | `bEnableClothCollision=true` 热切换；`ContactFriction=0.35` 作用于全部有效碰撞体；预设应用恢复 `0.35`，Details 手动改值进入 Custom 并要求 Reset | 静态核对完成 |
| Capacity/upload | 五个 collider buffer 始终填充到 64；`Params3.w` 为实际有效数；超过 64 明确失败，减少后仍需 Reset，暂停/单步不可绕过 | 静态核对完成；RHI 待验收 |
| Pose history | World 时间记录；substep 与 SceneWind 使用同一 offset；位置 linear、旋转最短弧 slerp；速度为 m/s、rad/s；新增/重新启用/形状或物理尺寸改变/显式 reset 零速度播种；Plane preview extent 不改变物理历史 | 静态核对完成 |
| Preview lifecycle | 开始/停止为 CallInEditor 中文按钮；普通 Editor World 才接受预览请求；World 帧去重；pause/reset/step 共用原求解路径 | 静态核对完成；Editor 待验收 |
| Render/editor safety | renderer transient，源网格 temporary editor-hidden；停止恢复 source；realtime override 与 throttle 成对；save/PIE suspend，结束后从 Rest；重构恢复意图而非 GPU 历史 | 静态核对完成；生命周期实测待验收 |

静态证据：

- 对实施前快照与更新目录执行 `git -c core.autocrlf=false diff --no-index --check`，无空白错误输出。`--no-index` 返回 1 表示两个目录有改动，不是构建结果。
- 比对 16 个物理核心、Shader、Bake/资产数据、Scene Proxy、Vertex Factory 和内部 Render Component 文件，SHA-256 均与实施前一致；没有新增/改写求解 Shader 或改变 16 字节 Diagnostics ABI。
- 逐子步数据流为不可变快照 → `SampleCollider` → 原有 `UpdateColliderInputs` → 五组 64 槽数组 → RDG scatter upload pass → 原接触 Shader。动态上传不使用图开始时的初始上传来替代子步更新，亦不覆盖 GPU SceneWind。
- 核对 UE 5.7.4 本地源码中的保存/PIE delegate 签名、viewport realtime override、Slate throttle API、临时隐藏 API 与组件重建时对 `CPF_Transient` 的跳过规则。
- 正式同步只允许 TARibbon 文件，覆盖前检查实施前哈希；不修改 Engine 源码、SceneWind、资产、Binaries 或 Intermediate。

新增但未执行的自动化测试（`Source/TARibbon/Private/Tests/TARibbonCollisionTests.cpp`）：

- `TA.Ribbon.Collision.GeometryAndUnits`：三种形状、单面法线、单位、正均匀缩放和非法尺寸。
- `TA.Ribbon.Collision.SubstepTrajectories`：插值、角速度、端点、重置、同时间戳更新、历史裁剪、倒退时间、四元数符号与 30/60/120 FPS 记录轨迹。
- `TA.Ribbon.Collision.FixedCapacityAndRemoval`：0→1→多个→64→0、五缓冲补零、65 超限、关闭清空、保护 State0/FaceWind、预设摩擦。
- `TA.Ribbon.Collision.ProjectionPinsAndFriction`：CPU 参考三形状投影、固定点、无弹跳、有界摩擦和平移/旋转表面速度传递。

## B. 手动场景检查（需 Editor/PIE，当前未执行）

建立一个最小 World：单个已 Bake v2 的 ribbon、一个 `UTARibbonColliderComponent`，再按项逐一记录现象和日志，不以“看起来正常”代替证据。

1. 接触：Sphere、Capsule、Plane 分别放在 ribbon 路径上，检查穿透修正、单侧 Plane（local +Z 允许侧）和 `ContactFriction=0.35`；关闭 `bEnableClothCollision` 后确认下一帧不再施加碰撞。
2. 帧与速度：移动/旋转碰撞体，观察 substep 采样是否连续；记录线速度单位换算和角速度方向，不将瞬时跳变误记为连续轨迹。
3. 形状排序/移除：添加多个碰撞体，核对按组件路径的稳定顺序；禁用、删除、重新注册和蓝图重构后确认无悬挂条目，重建不应无故清空仍存组件的历史。
4. 预览：在普通 Level Editor viewport 使用“开始编辑器预览”，验证暂停、Reset、单步；确认 Blueprint viewport 不启动该模拟。
5. 保存与 PIE：预览运行时保存关卡、进入/退出 PIE，确认预览暂停、源网格可见性恢复/再隐藏，返回后从 Rest 重建而非继续旧 GPU 历史。
6. 撤销/重做与蓝图重构：修改碰撞体形状、球/胶囊物理尺寸、启用状态并 Undo/Redo；执行 Construction Script 重构，确认预览意图和组件路径恢复，旧临时渲染器不泄漏。
7. World 隔离：在两个 World 各放 ribbon 与 collider，确认不会互相接触、不会共享排序或历史；Specific SceneWind 仍遵守同 World 约束。
8. 阴影/可见性：检查 transient renderer 的源网格隐藏/恢复、动态阴影开关和 Bounds；记录实际视口/PIE 结果，不把线框颜色当作阴影通过。
9. 容量与无效输入：构造负/零/非均匀缩放、非法尺寸、65 个启用碰撞体；确认无效项被排除、超限项失败并留下可解释诊断。

## C. 证据与结论边界

- 手动场景检查全部待执行。源码静态核对是本轮唯一完成的验证层级。
- 新增 automation tests 尚未编译或执行；不能把测试代码存在当作数值验证结果。
- 未有 Editor/PIE/RHI/GPU/性能证据前，不下“接触稳定”“预览可用”“帧率达标”“动态阴影通过”等结论。
