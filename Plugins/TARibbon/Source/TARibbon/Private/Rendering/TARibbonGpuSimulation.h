#pragma once

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RibbonGpu.h"
#include "SceneWindFieldEvaluationParameters.h"

#include <vector>

struct FTARibbonCollisionSnapshot;

enum class ETARibbonGpuFailureCode : uint32
{
	None = 0,
	MissingInitialUpload = 1,
	ResourceCreationFailed = 2,
	ResetBeforeCreate = 3,
	ResetUploadFailed = 4,
	RenderStateInvalid = 5,
	CollisionInputInvalid = 6
};

/**
 * Persistent render-facing buffers.  The object is intentionally independent of
 * any SceneProxy or vertex factory: consumers register these pooled buffers with
 * their own graph and create SRVs for the current draw. Positions remain absolute
 * UE-world meters; the consumer owns the meters-to-component-local-centimeters
 * conversion.
 *
 * Only the atomics are cross-thread status. The pooled-buffer references are
 * render-thread-owned and must be queried there after IsReady() publication;
 * game-thread diagnostics must not inspect TRefCountPtr members.
 */
struct FTARibbonRenderState
{
	TRefCountPtr<FRDGPooledBuffer> RenderPosition;
	TRefCountPtr<FRDGPooledBuffer> RenderPreviousPosition;
	TRefCountPtr<FRDGPooledBuffer> RenderNormal;
	TRefCountPtr<FRDGPooledBuffer> RenderTangent;
	TRefCountPtr<FRDGPooledBuffer> Diagnostics;
	uint32 RenderVertexCount = 0;
	uint64 Revision = 0;
	TAtomic<bool> bReady = false;
	TAtomic<bool> bFailed = false;
	TAtomic<uint32> FailureCode = 0;
	TAtomic<uint32> LatestBadTriangle = 0;
	TAtomic<uint32> LatestBadHinge = 0;
	TAtomic<uint32> LatestWindClamped = 0;
	TAtomic<uint32> LatestBadContact = 0;
	TAtomic<bool> bDiagnosticsValid = false;

	bool IsValid() const
	{
		return RenderPosition.IsValid() && RenderPreviousPosition.IsValid()
			&& RenderNormal.IsValid() && RenderTangent.IsValid() && Diagnostics.IsValid();
	}

	bool IsReady() const { return bReady.Load(EMemoryOrder::SequentiallyConsistent); }
	bool HasFailed() const { return bFailed.Load(EMemoryOrder::SequentiallyConsistent); }
	uint32 GetFailureCode() const { return FailureCode.Load(EMemoryOrder::SequentiallyConsistent); }
	const TCHAR* GetFailureMessage() const
	{
		switch (static_cast<ETARibbonGpuFailureCode>(GetFailureCode()))
		{
		case ETARibbonGpuFailureCode::MissingInitialUpload: return TEXT("Missing initial upload");
		case ETARibbonGpuFailureCode::ResourceCreationFailed: return TEXT("Resource creation failed");
		case ETARibbonGpuFailureCode::ResetBeforeCreate: return TEXT("Reset requested before Create");
		case ETARibbonGpuFailureCode::ResetUploadFailed: return TEXT("Reset upload failed");
		case ETARibbonGpuFailureCode::RenderStateInvalid: return TEXT("Render state is invalid");
		case ETARibbonGpuFailureCode::CollisionInputInvalid: return TEXT("Collision input is invalid");
		default: return TEXT("");
		}
	}

	bool TryGetLatestDiagnostics(FUintVector4& OutDiagnostics) const
	{
		if (!bDiagnosticsValid.Load(EMemoryOrder::SequentiallyConsistent))
		{
			return false;
		}
		OutDiagnostics = FUintVector4(
			LatestBadTriangle.Load(EMemoryOrder::SequentiallyConsistent),
			LatestBadHinge.Load(EMemoryOrder::SequentiallyConsistent),
			LatestWindClamped.Load(EMemoryOrder::SequentiallyConsistent),
			LatestBadContact.Load(EMemoryOrder::SequentiallyConsistent));
		return true;
	}
};

struct FTARibbonGpuCreateRequest
{
	const taribbon::gpu::Upload* InitialUpload = nullptr;
};

struct FTARibbonGpuResetRequest
{
	const taribbon::gpu::Upload* ResetUpload = nullptr;
};

struct FTARibbonGpuFrameRequest
{
	// Required when PrebuiltTickPlans is null. The simulation layer only reads
	// these values to build the repository's existing command plan.
	const taribbon::CookedMesh* CookedMesh = nullptr;
	const taribbon::SimulationConfig* Config = nullptr;

	// The caller owns this persistent CPU-side upload description. It is mutated
	// by SampleExternalInputs and is never used to overwrite State0/State1.
	taribbon::gpu::Upload* UploadState = nullptr;

	uint32 FixedTickCount = 0;
	double StartTime = 0.0;

	// Value-only SceneWind snapshot captured on the game thread. When disabled or
	// invalid, every SampleExternalInputs stage explicitly clears FaceWind.
	bool bUseSceneWind = false;
	FSceneWindFieldEvaluationParameters SceneWindParameters;
	double SceneWindTimeOffset = 0.0;
	TSharedPtr<const FTARibbonCollisionSnapshot, ESPMode::ThreadSafe> CollisionSnapshot;

	// Optional prebuilt plans. When present, the layer does not rebuild or reorder
	// them. Each entry corresponds to one fixed tick.
	const std::vector<std::vector<taribbon::gpu::Command>>* PrebuiltTickPlans = nullptr;
	const std::vector<taribbon::gpu::Command>* PrebuiltRenderPlan = nullptr;
	uint32 RenderVertexCount = 0;

	using FSampleExternalInputs = TFunction<void(double, taribbon::gpu::Upload&)>;
	FSampleExternalInputs SampleExternalInputs;
};

/**
 * UE/RDG bridge for the repository's existing taribbon::gpu upload and command
 * plan.  All passes are ordinary ERDGPassFlags::Compute passes (never async).
 * Every command in a plan becomes one dispatch; RDG resource dependencies provide
 * the required write/read ordering while preserving the plan's color ordering.
 */
class FTARibbonGpuSimulation final
{
public:
	FTARibbonGpuSimulation();
	~FTARibbonGpuSimulation();

	FTARibbonGpuSimulation(const FTARibbonGpuSimulation&) = delete;
	FTARibbonGpuSimulation& operator=(const FTARibbonGpuSimulation&) = delete;

	bool Create(FRDGBuilder& GraphBuilder, const FTARibbonGpuCreateRequest& Request, FString* OutError = nullptr);
	bool Reset(FRDGBuilder& GraphBuilder, const FTARibbonGpuResetRequest& Request, FString* OutError = nullptr);
	bool AddFrame(FRDGBuilder& GraphBuilder, const FTARibbonGpuFrameRequest& Request, FString* OutError = nullptr);
	/** Enqueues a non-blocking diagnostics copy. PollDiagnostics may be called later;
	 * returns false with no error while the previous copy is still pending. */
	bool EnqueueDiagnosticsReadback(FRDGBuilder& GraphBuilder, FString* OutError = nullptr);
	/** Returns false while the GPU copy is pending; never waits for the GPU. */
	bool PollDiagnostics();

	void Release();
	bool IsCreated() const { return bCreated; }
	TSharedPtr<FTARibbonRenderState, ESPMode::ThreadSafe> GetRenderState() const { return RenderState; }

private:
	struct FResources;

	bool CreateResources(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError);
	bool QueueInitialUpload(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError) const;
	bool QueueResetUpload(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError) const;
	bool QueueDynamicInputs(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, bool bIncludeImpulse, FString* OutError) const;
	bool AddSceneWindPass(FRDGBuilder& GraphBuilder, const TMap<FName, FRDGBufferRef>& Registered,
		const FSceneWindFieldEvaluationParameters& Wind, uint32 TriangleCount,
		double SampleTimeSeconds, FString* OutError) const;
	bool AddPlan(FRDGBuilder& GraphBuilder, const std::vector<taribbon::gpu::Command>& Plan,
		taribbon::gpu::Upload& Upload, const FTARibbonGpuFrameRequest::FSampleExternalInputs& Sampler,
		const FTARibbonGpuFrameRequest* FrameRequest, FString* OutError,
		uint32 RenderVertexCountOverride = 0, bool bOverrideRenderVertexCount = false);
	bool AddRenderPlan(FRDGBuilder& GraphBuilder, const std::vector<taribbon::gpu::Command>& Plan,
		taribbon::gpu::Upload& Upload, uint32 RenderVertexCount, FString* OutError);
	void MarkRenderOutputsForExternalRead(FRDGBuilder& GraphBuilder) const;

	TUniquePtr<FResources> Resources;
	TSharedPtr<FTARibbonRenderState, ESPMode::ThreadSafe> RenderState;
	TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> DiagnosticsReadback;
	bool bCreated = false;
};
