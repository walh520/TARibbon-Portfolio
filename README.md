# TARibbon Dynamics

UE GPU 单片布料系统

使用 GPU XPBD 模拟旗帜、挂布、薄片与单层连通网格，结合 Bake 数据、风场、简单形状碰撞和编辑器预览，形成从网格准备到实时形变的创作流程。

[GitHub 仓库](https://github.com/walh520/TARibbon-Portfolio) · [布料演示](https://www.bilibili.com/video/BV1n5eA6FEAH/) · [作品总集](https://www.bilibili.com/video/BV1MVak6jEPv/)

## 项目内容

项目面向 UE 5.7 系列，使用 5.7.4 项目 API。本仓库提供 Runtime / Editor 模块、Shader 与说明文档；集成所需的 UE 工程、配置和资产需另行准备。

演示视频来自完整作品工程，展示内容包含当前公开快照之外的依赖与资产。

## 实现与贡献

NiTong 负责 UE 插件架构、C++/HLSL 实现、GPU 资源调度、Bake v2 契约、编辑器预览、碰撞适配、诊断与验证工作流。求解与受力模型参考既有 XPBD、布料约束、空气动力学及数值方法，详细来源见 [ATTRIBUTION.md](ATTRIBUTION.md)。

## 核心功能

- **GPU XPBD**：使用持久 GPU 状态，支持固定步长、子步与约束迭代控制。
- **Bake v2**：为非平面、单片边连通且可定向的 Rest Mesh 建立求解数据。
- **顶点色控制**：R/G/B 分别表达硬固定、风响应和透风率。
- **风与接触**：接入 SceneWind / ArtWind，提供球体、胶囊、单面无限平面碰撞。
- **编辑器预览**：实现视口预览、暂停、重置与单步操作路径。
- **形变渲染**：公开版 Opaque/Masked 路径接入 Base/Depth/Velocity/VSM，并维护 Bounds 与速度历史。

## 求解与架构

| 设计 | 实现方式 |
| --- | --- |
| GPU 持久状态与 RDG compute passes | 跨帧保留求解数据，按子步更新输入，减少完整状态的 CPU 往返。 |
| 单片连通网格与 Bake v2 | 明确网格拓扑、固定区域与风响应语义，聚焦单片布料。 |
| 独立形状碰撞组件 | 以球体、胶囊和平面提供可控的接触输入。 |
| 共用求解路径的编辑器预览 | 通过同一求解流程支持重置、暂停与单步检查。 |

Shader 使用 XPBD 标量投影 `Δλ=(-C-α̃λ)/(Σw|∇C|²+α̃)`，柔度按子步时间平方缩放。求解依次更新三角 U/V 长度应变与剪切，再处理二面角弯曲；颜色批次用于隔离共享顶点的写入冲突。

## 本地材质扩展

公开版材质要求为 Opaque/Masked 与 Two Sided。本地版本在保持 GPU 求解和 Shader 本体一致的基础上，扩展材质校验与 SceneProxy，接受基础半透明混合模式，并继续要求 Two Sided。

本地透明分支沿用 UE 基础透明路线，未加入三角形排序或 OIT。`GetViewRelevance` 的 Velocity 条件仍限定 Opaque；透明褶皱排序、TSR/运动模糊及 VSM 透射/不透明度阴影尚待验证。

材质透明度与物理透风率分开控制，透风率仍由顶点色 B 决定。该材质扩展尚未包含在当前公开快照中。

## 代码阅读入口

1. [TARibbonComponent.h](Plugins/TARibbon/Source/TARibbon/Public/TARibbonComponent.h) 与 [WorldSubsystem](Plugins/TARibbon/Source/TARibbon/Public/TARibbonWorldSubsystem.h)：组件与调度入口
2. [TARibbonGpuSimulation.cpp](Plugins/TARibbon/Source/TARibbon/Private/Rendering/TARibbonGpuSimulation.cpp) → [RibbonKernels.usf](Plugins/TARibbon/Shaders/Private/RibbonKernels.usf)：资源、子步与求解
3. [TARibbonMeshData.cpp](Plugins/TARibbon/Source/TARibbon/Private/TARibbonMeshData.cpp)：Bake 数据
4. [ColliderComponent](Plugins/TARibbon/Source/TARibbon/Public/TARibbonColliderComponent.h) 与 [EditorModule](Plugins/TARibbon/Source/TARibbonEditor/Private/TARibbonEditorModule.cpp)：碰撞及预览生命周期
5. [设置与创作说明](Plugins/TARibbon/Docs/TARibbon_V1_Setup_CN.md)、[V1.3 验证清单](Plugins/TARibbon/Docs/TARibbon_V13_Verification_CN.md)

## 依赖与使用

- UE 5.7.4 项目 API
- 项目插件 SceneWind；正式 Toon 工程配置还间接依赖 TA_WorldInteraction
- 兼容的 UE 工程、配置、资产与引擎安装，详见 [DEPENDENCIES.md](DEPENDENCIES.md)

在依赖齐全的项目中集成 `Plugins/TARibbon` 并构建，按 [Setup](Plugins/TARibbon/Docs/TARibbon_V1_Setup_CN.md) 准备网格：

1. 使用细分网格，LOD0 为单 Section、单材质槽，关闭 Nanite
2. 设置源网格为 Movable，Scale 为 `(1,1,1)`，材质启用 Two Sided
3. 用顶点色 R 设置硬固定遮罩，同时保留固定区和自由区
4. 将 UTARibbonComponent 挂到可见网格所在 Actor，完成 Bake v2 后检查预览与碰撞

碰撞体使用正均匀缩放；V1.3 的碰撞体容量为 64，超过容量时显式报错。

## 验证状态

公开快照与本地透明扩展尚未执行自动化测试、UBT/UHT、ShaderCompileWorker、Cook、Editor/PIE、GPU Capture、视觉及性能验收。测试文件和验证清单已随源码提供，编译与运行结果仍待补充。

重点验证项包括接触表现、保存/PIE/重建时的预览生命周期、各渲染通道与透明材质表现。当前没有实测性能数据；后续记录需注明网格规模、子步/迭代、碰撞体数、GPU 型号与各通道帧时。

## 来源与许可

以 [LICENSE-SOURCE-AVAILABLE.txt](LICENSE-SOURCE-AVAILABLE.txt) 所列的源码可阅（source-available）条款发布。算法来源、Epic API 与 SceneWind 依赖说明见 [ATTRIBUTION.md](ATTRIBUTION.md)。
