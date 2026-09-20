#pragma once

#include "CoreMinimal.h"
#include "RibbonGpu.h"
#include "TARibbonCollision.h"
#include "Rendering/TARibbonGpuSimulation.h"

#include <vector>

class UTARibbonMeshData;

namespace UE::TARibbon::Private
{
	/** Applies Bake v2 G/B masks to TriRest4.xy by a three-vertex face average. */
	bool ApplyPaintedAeroMasks(
		const UTARibbonMeshData& MeshData,
		taribbon::gpu::Upload& Upload,
		FString* OutError = nullptr);

	/** Aligns the final executed fixed substep to the current world time. */
	double ComputeSceneWindTimeOffset(
		double WorldTimeSeconds,
		double RequestStartTime,
		uint32 FixedTickCount,
		double FixedDt);
}

/** Immutable cook/config plus the host-input upload image retained for one component instance. */
struct FTARibbonPreparedSimulation
{
	taribbon::CookedMesh CookedMesh;
	taribbon::SimulationConfig Config;
	taribbon::gpu::Upload UploadState;
	std::vector<taribbon::gpu::Command> RenderPlan;
};

/** Game-thread packet consumed in FIFO order by the render-thread world update. */
struct FTARibbonWorldDispatchRequest
{
	TSharedPtr<FTARibbonGpuSimulation, ESPMode::ThreadSafe> Simulation;
	TSharedPtr<FTARibbonPreparedSimulation, ESPMode::ThreadSafe> Prepared;
	uint32 FixedTickCount = 0;
	double StartTime = 0.0;
	bool bUseSceneWind = false;
	FSceneWindFieldEvaluationParameters SceneWindParameters;
	double SceneWindTimeOffset = 0.0;
	TSharedPtr<const FTARibbonCollisionSnapshot, ESPMode::ThreadSafe> CollisionSnapshot;
	bool bCreate = false;
	bool bReadbackDiagnostics = false;
};
