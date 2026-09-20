# TARibbon V1.3 中文设置与验证指南

本文对应正式项目 `Plugins/TARibbon` 中的 TARibbon V1.3 实现（UE 5.7.4）。TARibbon 组件是逻辑控制组件：它必须挂到一个已经存在的、合规的单张连通 `StaticMeshComponent`，该网格仍是唯一的 Mesh、材质和顶点颜色 authoring 来源。组件运行时可用内部临时渲染器替代原网格的绘制，但不会在场景资产中新增第二份 Mesh/材质入口。

本文只描述设置契约、静态检查和未验证边界；不把本文或本次文档更新表述为编译、Cook、PIE、GPU 或画面验收通过。

## 1. 合规网格与组件放置

### 1.1 网格要求

源网格必须满足以下条件：

- 只使用 LOD0、一个 section / polygon group 和一个材质槽；关闭 Nanite。
- 三角形必须组成一个边连通组件，允许有边界；不得有重合点、退化三角形、非流形边、非流形边界 link 或不可一致定向的共享边。
- 网格可以是平面，也可以是非平面。非平面 Rest Mesh 或预弯形状是受支持的；不要为了满足旧版规则把 Rest 形状强行压平。
- 至少有一个固定点和一个自由点。材质建议使用 Two Sided，Blend Mode 使用 Opaque 或 Masked。
- 目标组件的 Mobility 使用 Movable，Scale 保持 `(1,1,1)`；需要动态阴影时开启 Cast Shadow 和 Cast Dynamic Shadow。

### 1.2 挂载逻辑组件

1. 把源网格作为正常的 `StaticMeshComponent` 放进 Actor。
2. 添加 `TA Ribbon Component`（`UTARibbonComponent`），并将它拖到目标 `StaticMeshComponent` 下面作为子组件。这是多个网格组件共存时的明确选择方式。
3. 如果 Actor 中只有一个有效的 `StaticMeshComponent`，也可以把 TARibbon 放在 Actor 上，由组件自动识别该唯一源网格。
4. 如果 Actor 有多个有效 `StaticMeshComponent`，必须把 TARibbon 挂到目标网格下面；否则验证会拒绝不明确的源网格。
5. 组件上依次执行 `Bake Ribbon Data` 和 `Validate Ribbon Setup`。诊断中应能读到 LOD0 的 simulation vertex、triangle、hinge、固定点以及颜色域计数。

TARibbon 的源组件选择规则是“挂载父级优先；否则 Actor 必须恰好有一个有效源网格”。不要把 TARibbon 当作第二个可见网格，也不要给它另设一套材质。

## 2. 布料碰撞体

`UTARibbonColliderComponent` 是独立的 `UPrimitiveComponent`，只提供碰撞形状、尺寸、启用状态、几何诊断、线框预览和重置运动历史操作；它不拥有 Mesh、材质或任何 authored mesh 字段，也不创建 Chaos body。它通过同一 World 的 TARibbon world subsystem 提供单向布料碰撞输入。

在需要推动布料的 Actor 上添加“TA Ribbon 布料碰撞”，选择形状并调整组件位置和尺寸，使绿色线框贴合外形。一个 Actor 可以组合多个碰撞组件；不会自动读取 Actor 模型的三角面。随后在布料组件点击“开始编辑器预览”，拖动碰撞 Actor 观察接触。新 C++ 组件需在后续正常构建并重新加载插件后使用；本轮不启动编译。

### 2.1 形状、单位与默认值

- `Sphere / 球体`：半径 `50 cm`。
- `Capsule / 胶囊`：半径 `50 cm`，胶囊半高 `100 cm`；半高从中心量到两端最外侧，包含两端半球，轴线为组件局部 `+Z`。
- `Plane / 平面（单侧）`：碰撞点为组件原点，法线为组件局部 `+Z`；局部 `+Z` 是允许侧。`PlanePreviewExtentCm=100 cm` 只决定编辑器方框线框，物理平面仍为无限平面。

只接受有限的正均匀缩放；负、零或非均匀缩放会使诊断无效。无效碰撞体显示红色、计入无效诊断但不进入物理快照；不要依赖“自动拟合”来掩盖错误输入。

### 2.2 布料接触开关与容量

- `bEnableClothCollision` 默认为 `true`，是布料侧的热开关，下一帧生效。
- `ContactFriction` 默认为 `0.35`，作用于该布料解析到的全部有效碰撞体。碰撞体启用/禁用不改变其 authored 几何。
- 当前实现没有 Chaos 反馈、CCD、自碰撞、Box、Triangle Mesh 或双向刚体碰撞；接触只在 TARibbon 布料求解器内单向投影。
- 上传始终写入 `ColliderMeta`、`ColliderGeometry0/1`、`ColliderMotion0/1` 五个数组并填充到容量 `64`；实际 `active count` 仍是当前有效碰撞体数。超过 64 个启用且有效的碰撞体会报错，必须先减少数量，再执行 Reset；暂停/恢复和单步不能绕过错误。
- 每次约束迭代后投影接触，子步结束按碰撞体表面速度消除向内相对法向速度并施加有界摩擦。硬固定点不参与移动，默认无弹跳；保留布料厚度和 `1e-7 m` 接触容差。当前是离散顶点接触，展示网格要充分细分，高速穿越不能保证不穿透。

### 2.3 运动历史与采样

World subsystem 按组件路径稳定排序采集碰撞体，并以 World 时间记录姿态历史。布料每个 fixed substep 按与 SceneWind 相同的时间偏移关系采样；位置线性插值、旋转使用最短弧 `slerp`，线速度使用世界 `cm/s` 转为求解器 `m/s`，角速度为世界 `rad/s`。新增、重新启用、形状/球胶囊物理尺寸改变或执行“重置碰撞运动历史”都会清空旧轨迹并以零速度单点重新播种；Plane 的 preview extent 不是物理尺寸。Construction Script 同一轮重建通过 Actor 身份与组件名称衔接记录。

零物理 Tick 或布料暂停的帧仍记录碰撞体姿态，历史覆盖最大追赶区间；区间外保持最近端点且速度为零（端点时间有 `1e-8 s` 舍入容差）。普通移动碰撞 Actor 不需要重置布料；瞬移时可主动重置碰撞运动历史。采样时刻为 `Command.sampleTime + SceneWindTimeOffset`，本帧最后一个执行子步与当前 World 时间对齐，丢弃追赶 Tick 后不会永久落后。

编辑器线框颜色：有效且启用为绿色，已禁用为灰色，无效为红色。

## 3. Blender 点域 Byte Color authoring

### 3.1 建立属性

在 Blender 的 Object Data Properties（绿色三角形）> Color Attributes 中新建：

- Name：`TARibbon_Pin`
- Domain：`Point`
- Data Type：`Byte Color`

`Point` 是 authoring 约定。导入后 UE 可能因 UV seam 或硬法线把一个点展开为多个 Vertex Instance；这些副本必须保持同一组 RGB 值，不能在 seam 两侧画出冲突数据。

### 3.2 RGB 含义与默认值

| 通道 | 含义 | 有效范围与说明 |
| --- | --- | --- |
| R | 硬固定（pin） | 近似二值；`R <= 0.05` 为自由，`R >= 0.95` 为固定，运行时以 `R >= 0.5` 判定 |
| G | 风响应权重 | `[0,1]`；`0` 不响应 SceneWind / ArtWind，`1` 为完整响应 |
| B | 渗透率 | `[0,1]`；`B=1` 表示无气动响应，`B=0` 表示不因渗透率削弱风响应 |
| A | 保留 | 不参与 TARibbon V1.3 的固定、风响应或渗透率判定 |

建议先将全部点设为自由默认值 `(R=0, G=1, B=0, A=1)`，再把固定点设为 `(R=1, G=1, B=0, A=1)`。因此：

- 自由默认：`(0,1,0,1)`；
- 固定默认：`(1,1,0,1)`。

G、B 可按点渐变或分区绘制，但必须保持在 `[0,1]`。R 不要使用中间灰度。固定点和自由点都必须存在；全自由或全固定都会被 Bake 拒绝。

### 3.3 形状与初态

网格可以在 Blender 中预弯、起伏或做其他非平面 Rest 形状；这就是模拟的初始几何，不是运行后才附加的位移。若 Rest 形状完全平坦、重力与所有初始面几乎切向、且没有有效风，诊断会给出“对称初态”警告：理想对称状态可能保持平面，不应误判为求解器没有工作。处理方式是给 Rest Mesh 一个轻微、明确的曲率，或旋转初始姿态；也可以确认确实存在有效 SceneWind/ArtWind 输入。

## 4. FBX 导入与 Bake v2 迁移

### 4.1 FBX 导入

导出 FBX 时保留 Mesh、Normals/Tangents、UV0 与 Vertex Colors。UE Static Mesh 导入设置至少确认：

- `Vertex Color Import Option`：`Replace`；
- 只保留一个 source LOD（LOD0）；
- Nanite：关闭；
- 一个 section / polygon group、一个材质槽；
- 材质使用 Two Sided，Blend Mode 为 Opaque 或 Masked。

必须使用 `Replace`，让本次 FBX 中的 `TARibbon_Pin` 颜色成为 MeshDescription 的当前颜色。`Ignore` 可能保留旧数据，不能作为可重复的 V1.3 authoring 流程；若颜色未导入，Bake 会把默认全白识别为“没有 authored/imported vertex color data”，而不是把它当成有效 pin mask。

### 4.2 从旧数据迁移到 Bake v2

当网格、拓扑、颜色或顶点域发生变化时，按以下顺序迁移：

1. 在 Blender 更新 `TARibbon_Pin` 与 Rest 形状，导出 FBX。
2. 在 UE 对同一 Static Mesh 使用 `Replace` 重导入。
3. 在挂载的 TARibbon 组件上执行 `Bake Ribbon Data`，生成/覆盖 `BakeVersion=2` 的 LOD0 Asset User Data。
4. 执行 `Validate Ribbon Setup`，确认固定点、风响应、渗透率、三角形与渲染绑定数量有效。
5. 保存 Static Mesh 资产；进入游戏模拟世界前再按第 8 节规则 Reset。

Bake v2 会写入 LOD0 物理顶点、三角形、固定点、每点风响应、每点渗透率、UV0、render bindings、render indices 和 topology signature。旧版组件上的单一 `Permeability` 参数不再是 authoring 来源；迁移时应把渗透率写入 Vertex Color B 后重新 `Replace` + Bake，不能只改旧参数。拓扑签名过期、颜色冲突、缺少 UV0/切线基、LOD/section 不符合要求时，Bake 应停止并保留原资产，不要跳过错误继续运行。

## 5. SceneWind：放置、路由与单位

在场景中放置 `AWind` / SceneWind Actor，使它的 Field Size 覆盖需要受风的 ribbon 区域，并确认它注册为 SceneWind。SceneWind 的覆盖范围由其 XY bounds 定义；采样点落在范围外时物理风明确清零，不会把边界外的值外推进来。若使用 `Active Scene Wind`，场景中应有可解析的 active Wind Actor；若需要局部或确定的风源，使用 `Specific Scene Wind` 并指定同一 World 中的 Wind Actor。

SceneWind 设置使用 UE 世界单位：位置、Field Size、波长使用厘米（cm），风速使用厘米/秒（cm/s）。TARibbon 布料求解使用 SI 单位：Rest 位置在内部转换为米，风与速度使用 m/s，加速度限制使用 m/s²；不要把 `500 cm/s` 误填成 `500 m/s` 的同量级。

风不是每帧只取一次的刚体常量。每个 fixed substep，TARibbon 对当前三角形质心重新采样 SceneWind；因此 ribbon 变形后质心移动会改变其空间风输入。域外质心的 SceneWind 速度为零，但 ArtWind 是否启用仍按组件自身设置处理。

### 5.1 三种 Wind Source Mode

| 组件选项 | 风源解析 | 放置与失败行为 |
| --- | --- | --- |
| `Active Scene Wind` | 从当前 World 的 SceneWind subsystem 取得 active Wind Actor | 将 AWind 放在同一 World 并注册为 active；没有有效 active 源时，物理 SceneWind 为零并在 Diagnostics 给出 warning |
| `Specific Scene Wind` | 只使用 `SpecificWindActor` 指定的 AWind | 指定 Actor 必须有效且属于同一 World；未指定、跨 World 或参数快照无效时，物理 SceneWind 为零并给出 warning |
| `Disabled` | 不解析 SceneWind | SceneWind 贡献为零；如果启用了 ArtWind，ArtWind 仍可独立工作 |

切换 Active/Specific/Disabled 或更换 `SpecificWindActor` 属于风源路由热切换：运行中会刷新风源诊断，不要求为此重建布料历史。热切换仍要检查 `Resolved Wind Source`、`SceneWind Warning` 与当前模式，避免把“无源导致零风”误认为零风速调参结果。

## 6. 风安全参数与 ArtWind

- `MaxWindSpeed`（m/s）：限制合并后的 host SceneWind + ArtWind 风速；`0` 表示关闭该 safeguard。
- `AccelerationClamp`（组件字段：`WindAccelerationClamp`，单位 m/s²）：限制气动加速度；`0` 表示关闭该 safeguard。启用后可在 GPU diagnostics 观察 wind clamp 计数。
- `ArtWind` 是叠加在 SceneWind 之前的可控艺术风波。启用后最多配置 4 个 coherent waves；每个 wave 需要有效的速度方向、材质方向、振幅（m/s）、频率（Hz）、波长（m）和相位。
- 超过 4 个 ArtWind wave、方向/波长/参数非有限或超出有效范围时，验证失败。ArtWind 不会绕过 G 响应权重与 B 渗透率的最终气动削弱。

建议先关闭 ArtWind 验证 SceneWind 路由，再逐个增加 wave；否则同时存在 active 风、局部风和多条 ArtWind 时，诊断难以区分来源。

## 7. 默认求解参数

### 7.1 参数预设切换

组件的 `TA Ribbon|Preset` 分类提供 `Cloth Preset` 下拉框与“应用当前布料预设”按钮：

- 选择 `Cotton / 棉布`、`Silk / 丝绸` 或 `Canvas / 帆布` 后，会立即成组更新所有预设托管参数：面密度、U/V 拉伸、剪切、弯曲、阻尼、厚度、空气密度、法向/切向阻力、接触摩擦（重置为 `0.35`）、风速与气动加速度安全限制、固定步长、Substeps、Iterations、最大追赶 Tick，并关闭和清空旧 ArtWind。
- 手动修改任意一项上述托管参数，Details 面板中的预设会自动变为 `Custom / 自定义`。选择 Custom 本身不会覆盖当前值。
- “应用当前布料预设”用于把当前非 Custom 预设恢复到完整标准值；当前为 Custom 时按钮不改写参数。
- 预设不会改写 `WindSourceMode`、`SpecificWindActor`、`bEnableClothCollision`、Auto Start、Pause、Bounds、源 Mesh、材质或 Bake v2 顶点色 RGB。固定点、风响应和渗透率仍完全由模型顶点色控制。
- 模拟已初始化时切换/应用预设会明确要求执行 `Reset Simulation`；不会把新参数偷偷混入旧 GPU 历史。

| 预设 | 面密度 kg/m² | Stretch U / V | Shear | Bend N·m | Damping s⁻¹ | Thickness m | Normal / Tangent Drag |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 棉布 | 0.18 | 2200 / 1800 | 650 | 0.0045 | 1.1 | 0.002 | 1.18 / 0.14 |
| 丝绸 | 0.065 | 780 / 620 | 155 | 0.00055 | 0.55 | 0.002 | 1.08 / 0.10 |
| 帆布 | 0.32 | 4200 / 3600 | 1300 | 0.018 | 1.6 | 0.003 | 1.25 / 0.16 |

棉布和丝绸采用 TARibbon 指导仓库中的材料数值。指导仓库没有单独的帆布条目，因此帆布是本项目为快速展示准备的偏重、偏硬美术起点，并非材料实测标定。三个预设都把 Air Density 设为 `1.225 kg/m³`，安全限制设为 `0`，求解器设为 `1/60 s`、`8` Substeps、`2` Iterations、每帧最多 `4` 个 Tick。

### 7.2 初始默认值

| 参数 | 默认值 |
| --- | ---: |
| Areal Density | 0.18 kg/m² |
| Stretch U / V | 2200 / 1800 N/m |
| Shear | 650 N/m |
| Bend | 0.0045 N·m |
| Damping Rate | 1.1 s⁻¹ |
| Thickness | 0.002 m |
| Air Density | 1.225 kg/m³ |
| Normal / Tangent Drag | 1.18 / 0.14 |
| Contact Friction | 0.35 |
| Fixed Time Step | 1/60 s |
| Substeps / Solver Iterations | 8 / 2 |
| Max Ticks Per Frame | 4 |
| Auto Start | true |
| Bounds Expansion | 25 cm |

这些只是起点，不是对所有尺寸、材质和风速的通用结论。Bounds Expansion 是额外裁剪余量，不是稳定性参数；发生大位移裁剪时先合理增大它。

## 8. Reset、变换与运行中热切换规则

初始化时，LOD0 Rest 坐标通过源 `StaticMeshComponent` 当时的 Transform 转为世界米坐标，模拟历史固定在该初始化变换下。

- 源网格的位置、旋转或缩放改变后，不能继续沿用旧历史；组件会进入 Error。正确顺序是 Pause → 修改 Transform（Scale 仍为 1）→ `Reset Simulation` → Resume。
- 修改网格、顶点颜色、材质、布料参数（包括接触摩擦）、求解参数或 ArtWind 数组后，执行 `Reset Simulation`，让 GPU 状态和固定点目标按新输入重建。
- 没有编辑器预览请求时，`Reset Simulation` 在非 PIE 编辑器世界只做设置验证；开始编辑器预览后可在编辑器 World 重建其临时 GPU 状态，游戏模拟世界同样按请求重建。
- 运行中修改上述物理/源网格参数会报告“需要 Reset Simulation”的错误。`WindSourceMode` 与 `SpecificWindActor` 按第 5 节规则热切换；布料碰撞开关、碰撞体开关和碰撞体普通移动也允许热更新。Pause、诊断开关和 Bounds 余量不要求重新烘焙。
- `Pause` / 单步只改变模拟推进；`Reset` 会清除模拟历史并回到当前初始化 Rest 状态，不是对已有形变做连续修补。

### 8.1 编辑器预览

组件的 `CallInEditor` 操作按钮为“开始编辑器预览”和“停止编辑器预览”。它们只在普通 Level Editor 视口工作，不在 Blueprint viewport 启动模拟；现有“暂停 / 重置 / 单步”仍分别对应暂停推进、回到 Rest、排队一个 fixed tick。

预览使用 transient 临时渲染器，并以 `SetIsTemporarilyHiddenInEditor` 临时隐藏源网格；停止时恢复源网格原先的编辑器隐藏状态，不改写其持久可见性。编辑器模块只对拥有预览 World 的普通 Level Editor 视口添加 realtime override，并成对设置/解除 Slate throttle 豁免。保存或 PIE 开始时暂停并释放预览状态，保存完成或 PIE 结束后从源 Rest 重新建立；蓝图重构/撤销/重做若替换组件，也只恢复预览意图，不继承旧 GPU 历史。源布料网格 Transform 或物理参数变化仍要求 Reset；已存在的 Reset 错误不会通过保存/恢复自动消除。删除组件、切图和模块卸载会释放预览并恢复源网格显示。

## 9. 静态验证清单

在不运行编译、Cook 或 PIE 的前提下，可进行以下静态核对：

1. 文档、组件声明和 Bake v2 数据契约中的名称一致：`TARibbon_Pin`、`Active Scene Wind`、`Specific Scene Wind`、`Disabled`、`MaxWindSpeed`、`AccelerationClamp` / `WindAccelerationClamp`、`ArtWind`、`bEnableClothCollision`、`ContactFriction`。
2. Blender authoring 值满足 RGB 约定：自由 `(0,1,0,1)`、固定 `(1,1,0,1)`；R 二值，G/B 在 `[0,1]`；切换布料预设不会覆盖这些资产数据。
3. FBX 导入选项为 `Replace`，Static Mesh 为 LOD0/单 section/单材质槽/Nanite off。
4. 组件挂载关系能唯一解析一个合规源 `StaticMeshComponent`；网格三角形为单一边连通组件，Rest 形状可非平面。
5. SceneWind 的 Field Size 覆盖目标区域，且能解释域外质心为零风；运行中诊断应区分有效源、无源 warning 与 Disabled。
6. ArtWind wave 数量不超过 4；安全参数单位明确为 m/s 与 m/s²。
7. 变换或非路由参数变更后有 Reset 记录；对称平面初态警告有明确处理方案。
8. 每个 World 的碰撞体独立采集；有效/禁用/无效颜色和超过 64 容量的失败路径与组件诊断文字一致。

正式同步到插件文档目录后，可做差异空白检查（PowerShell）：

```powershell
git -C D:/UnrealProjects/Toon diff --check -- Plugins/TARibbon
```

若插件尚未被 Git 跟踪，上述命令不会覆盖未跟踪文件。本轮另以实施前快照与更新文件执行 `git diff --no-index --check`；完整记录见 `TARibbon_V13_Verification_CN.md`。

## 10. 未验证边界

本文和本次文档更新不声称以下项目通过：

- C++ 编译、UHT、链接、Shader 编译或 Derived Data 生成；
- Editor 启动、PIE、Cook、Automation、CPU/GPU 对照或 fixed-substep 一致性实测；
- GPU RDG dispatch、readback diagnostics、Scene Proxy 动态绘制、VSM 动态阴影或正反面画面；
- 非平面 Rest、SceneWind 域外清零、Active/Specific/Disabled 热切换、ArtWind 上限、Reset 后状态以及变换错误的运行时验收。
- 碰撞体接触、时间插值/速度、形状排序与移除、预览恢复、保存/PIE、撤销/重做、蓝图重构、World 隔离和动态阴影的运行时验收；五个碰撞数组的实际 RHI 内容与性能数据。

上述项目需要在具备对应构建和 DX12 Editor/PIE 条件的环境中单独验证；静态文档核对不能替代这些证据。
