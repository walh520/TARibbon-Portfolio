# TARibbon Dynamics

UE GPU 单片布料系统

> GPU XPBD 单片布料源码快照；本地增加基础透明材质路径，透明排序、阴影及专用 Velocity 不作保证。

[GitHub 仓库](https://github.com/walh520/TARibbon-Portfolio)

## 简介与公开范围

面向 UE 5.7 系列、项目 API 对应 5.7.4 的 GPU 单片布料系统，适用于旗帜、挂布、薄片和单层连通网格。本仓库是 Runtime / Editor 模块、Shader 与说明文档的源码快照，不是完整 Unreal 工程。

## 本地工程与公开快照

公开插件树的 42 个文件中，38 个与本地相同、4 个内容不同；GPU 求解与 Shader 本体保持一致。差异集中于材质校验、SceneProxy、说明文档和描述文件。

公开版要求 Opaque/Masked 与 Two Sided；本地材质校验已接受基本半透明混合，并继续要求 Two Sided。当前本地透明分支沿用 UE 基础透明路线，没有三角形排序/OIT、透明阴影或专用透明 Velocity 保证。因此不能把公开 Opaque/Masked 路径的 Base/Depth/Velocity/VSM 覆盖无条件写到透明布料上。

## 演示

[布料演示](https://www.bilibili.com/video/BV1n5eA6FEAH/) · [作品总集](https://www.bilibili.com/video/BV1MVak6jEPv/)

视频可能来自依赖与资产齐全的作品工程，展示范围可以大于当前公开快照；不能用视频替代本快照的构建和验证记录。

## 实现与贡献

依据 [ATTRIBUTION.md](ATTRIBUTION.md)，NiTong 完成 UE 插件架构、C++/HLSL 实现、GPU 资源调度、Bake v2 契约、编辑器预览、碰撞适配、诊断与验证工作流。XPBD、布料约束、空气动力学及数值方法来自已有研究与公开方法，不作为个人发明。

## 核心功能

- GPU 持久状态下的 XPBD；固定步长、子步及约束迭代控制。
- 支持非平面、单片边连通且可定向 Rest Mesh 的 Bake v2 数据契约。
- 顶点色 R/G/B 分别表达硬固定、风响应、透风率。
- SceneWind / ArtWind 风输入；球体、胶囊、单面无限平面接触。
- 编辑器视口预览、暂停、重置、单步的实现路径。
- 公开 Opaque/Masked 路径将形变接入 Base/Depth/Velocity/VSM，并维护 Bounds 与速度历史；本地透明扩展不共享完整的通过性保证。

## 方案与取舍

| 选择 | 目的与边界 |
| --- | --- |
| GPU 持久状态与 RDG compute passes | 避免把每帧完整状态交回 CPU；需要管理跨帧资源与逐子步输入。 |
| 单片连通网格与 Bake v2 契约 | 收窄输入、明确固定与风响应语义，不承诺通用服装系统。 |
| 独立简单形状碰撞组件 | 支持可控的接触输入；不等同于任意场景碰撞或 Chaos 接触体系。 |
| 预览生命周期与正式求解路径共用 | 便于重置与单步检查；保存、PIE、重建等生命周期仍需实测。 |

本地透明支持放宽了材质输入，仍把求解与材质混合模式分开：透明度不改变物理透风率（顶点色 B）。`GetViewRelevance` 的 Velocity 条件仍要求 Opaque；透明褶皱前后排序、TSR/运动模糊及 VSM 透射/不透明度阴影不能推定正确。

实际 Shader 采用 XPBD 标量投影 `Δλ=(-C-α̃λ)/(Σw|∇C|²+α̃)`，柔度按子步时间平方缩放；依次更新三角 U/V 长度应变与剪切，再处理二面角弯曲。颜色批次隔离共享顶点冲突。这是求解结构说明，不保证任意参数下的稳定性或实测性能。

## 代码阅读入口

1. [TARibbonComponent.h](Plugins/TARibbon/Source/TARibbon/Public/TARibbonComponent.h) 与 [WorldSubsystem](Plugins/TARibbon/Source/TARibbon/Public/TARibbonWorldSubsystem.h)：组件与调度入口。
2. [TARibbonGpuSimulation.cpp](Plugins/TARibbon/Source/TARibbon/Private/Rendering/TARibbonGpuSimulation.cpp) → [RibbonKernels.usf](Plugins/TARibbon/Shaders/Private/RibbonKernels.usf)：资源、子步与求解。
3. [TARibbonMeshData.cpp](Plugins/TARibbon/Source/TARibbon/Private/TARibbonMeshData.cpp)：Bake 数据。
4. [ColliderComponent](Plugins/TARibbon/Source/TARibbon/Public/TARibbonColliderComponent.h) 与 [EditorModule](Plugins/TARibbon/Source/TARibbonEditor/Private/TARibbonEditorModule.cpp)：碰撞及预览生命周期。
5. [设置与创作说明](Plugins/TARibbon/Docs/TARibbon_V1_Setup_CN.md)、[V1.3 验证清单](Plugins/TARibbon/Docs/TARibbon_V13_Verification_CN.md)。

## 验证与性能

V1.3 验证文档明确：本轮只做源码实现与静态核对。自动化测试、UBT/UHT、ShaderCompileWorker、Cook、Editor、PIE、GPU Capture、视觉与性能验收均未执行。新增测试文件存在，不代表测试已编译或通过。

本次整理不重新运行 UE，不声称“接触稳定”“预览可用”“动态阴影通过”或“帧率达标”。后续应分别记录网格规模、子步/迭代、碰撞体数、GPU 型号与各通道帧时。

本轮仅比对当前实现与公开文件；未重跑原静态脚本，也没有 UE 构建、Shader 编译、Editor/PIE、透明视觉或性能验证。局部透明扩展说明明确保留这些验证缺口。

## 依赖与运行方式

- UE 5.7.4 项目 API；本包不含引擎源码。
- 项目插件 SceneWind；其正式 Toon 工程配置还间接依赖 TA_WorldInteraction。
- 还需兼容的 UE 工程、配置、资产与引擎安装，详见 [DEPENDENCIES.md](DEPENDENCIES.md)。

在依赖齐全的项目中集成 `Plugins/TARibbon`，再构建并验证。按 [Setup](Plugins/TARibbon/Docs/TARibbon_V1_Setup_CN.md) 使用细分、单 section 网格，设置顶点色 R 硬固定遮罩，将一个 UTARibbonComponent 挂到可见网格所在 Actor；完成 Bake v2 后再检查预览与碰撞。这是目标工程操作路径，当前公开快照没有独立构建通过证据。

本地输入校验仍要求源网格 Movable、Scale=(1,1,1)、LOD0 单 Section/单材质槽、Nanite 关闭，并具有固定区与自由区；Two Sided 对透明材质也适用。上述放宽不是任意布料/多材质/任意缩放支持。

## 限制与来源许可

适用范围是单片布料和明确输入契约。V1.3 记录的碰撞体容量为 64，超过容量显式失败；碰撞体要求正均匀缩放。接触、预览生命周期、VSM 与性能必须按原验证清单逐项实测。

以 [LICENSE-SOURCE-AVAILABLE.txt](LICENSE-SOURCE-AVAILABLE.txt) 发布；保留 [ATTRIBUTION.md](ATTRIBUTION.md) 的算法来源、Epic API 与 SceneWind 依赖说明。称为源码可阅快照，不统一称为开源。
