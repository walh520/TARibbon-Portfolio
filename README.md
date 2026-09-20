# TARibbon Dynamics — Unreal Engine GPU Cloth System

`TARibbon` is a GPU single-sheet cloth system for Unreal Engine 5.7. It provides a persistent XPBD simulation, GPU wind sampling, primitive collision contacts, editor preview support, and deformable rendering through the same render position used by the base, depth, velocity, and Virtual Shadow Map passes.

The implementation applies established XPBD, cloth-constraint, aerodynamic, and numerical methods. The project-specific work is the Unreal Engine plugin architecture, GPU resource orchestration, shader integration, mesh-bake contract, editor workflow, collision adapter, diagnostics, and validation structure.

## 中文简介

TARibbon 是一个面向 Unreal Engine 5.7 的 GPU 单片布料系统，适合旗帜、挂布、薄片和其他单层连通网格。系统通过 GPU 持久状态执行 XPBD 求解，并把模拟结果直接接入基础渲染、深度、速度和 Virtual Shadow Map。

主要能力包括：

- 固定步长、子步和约束迭代控制；
- 非平面单连通 Rest Mesh 的 Bake v2 数据契约；
- 顶点色 R/G/B 通道分别表达硬固定、风响应和透风率；
- SceneWind 与 ArtWind 风力输入；
- 球体、胶囊和单面无限平面碰撞体；
- 编辑器视口中的布料碰撞预览；
- 保守 Bounds、Velocity 历史和 VSM 动态阴影；
- CPU 参考数据、GPU diagnostics 和验证记录。

## Repository map

```text
Plugins/TARibbon/
├─ TARibbon.uplugin
├─ Source/TARibbon/              # Runtime module
├─ Source/TARibbonEditor/        # Editor preview module
├─ Shaders/Private/              # GPU compute and render shaders
└─ Docs/                         # Setup, authoring, and verification notes
```

## Source entry points

- `Plugins/TARibbon/Source/TARibbon/Public/TARibbonComponent.h`
- `Plugins/TARibbon/Source/TARibbon/Public/TARibbonColliderComponent.h`
- `Plugins/TARibbon/Source/TARibbon/Public/TARibbonWorldSubsystem.h`
- `Plugins/TARibbon/Source/TARibbon/Private/Rendering/TARibbonGpuSimulation.cpp`
- `Plugins/TARibbon/Shaders/Private/RibbonKernels.usf`
- `Plugins/TARibbon/Source/TARibbon/Private/TARibbonMeshData.cpp`
- `Plugins/TARibbon/Source/TARibbonEditor/Private/TARibbonEditorModule.cpp`

## Integration notes

The plugin expects the project-local `SceneWind` and `TA_WorldInteraction` modules described in `DEPENDENCIES.md`. A typical setup uses a subdivided, single-section static mesh, a hard-pin mask in vertex-color R, and one `UTARibbonComponent` attached to the actor that owns the visible mesh. Detailed mesh authoring, collision placement, parameter presets, and editor controls are documented under `Plugins/TARibbon/Docs/`.

## Verification status

The repository separates static source checks and test intent from build and runtime evidence. UBT/UHT, ShaderCompileWorker, Editor, PIE, GPU Capture, and visual or performance acceptance require execution in the target Unreal project; they must not be inferred from source presence. See `Plugins/TARibbon/Docs/TARibbon_V13_Verification_CN.md` for the recorded status.

## License and attribution

This source tree is released under the terms in `LICENSE-SOURCE-AVAILABLE.txt`. Attribution and third-party ownership information are listed in `ATTRIBUTION.md`.
