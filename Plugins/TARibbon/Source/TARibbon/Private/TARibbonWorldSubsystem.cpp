#include "TARibbonWorldSubsystem.h"

#include "TARibbonComponent.h"
#include "TARibbonColliderComponent.h"
#include "TARibbonCollision.h"
#include "TARibbonModule.h"
#include "TARibbonRuntimePrivate.h"

#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "CoreGlobals.h"

struct FTARibbonCollisionRegistry
{
	struct FKey
	{
		TWeakObjectPtr<AActor> Owner;
		FName ComponentName;
		bool operator==(const FKey& Other) const { return Owner == Other.Owner && ComponentName == Other.ComponentName; }
		friend uint32 GetTypeHash(const FKey& Key) { return HashCombine(GetTypeHash(Key.Owner), GetTypeHash(Key.ComponentName)); }
	};
	struct FHistory
	{
		FTARibbonColliderTrajectory Trajectory;
		uint64 MotionRevision = 0;
		TWeakObjectPtr<UTARibbonColliderComponent> LastComponent;
		bool bWasEnabled = false;
	};
	TMap<FKey, FHistory> Histories;
};

TSharedPtr<const FTARibbonCollisionSnapshot, ESPMode::ThreadSafe> UTARibbonWorldSubsystem::CaptureColliders(
	double TimeSeconds, double HistorySeconds)
{
	if (!CollisionRegistry) { CollisionRegistry = MakeShared<FTARibbonCollisionRegistry>(); }
	auto Snapshot = MakeShared<FTARibbonCollisionSnapshot, ESPMode::ThreadSafe>();
	Colliders.RemoveAll([this](const TWeakObjectPtr<UTARibbonColliderComponent>& Entry)
	{
		return !Entry.IsValid() || !Entry->IsRegistered() || Entry->GetWorld() != GetWorld();
	});
	// Object addresses and registration order are not a physical contact ordering.
	Colliders.Sort([](const TWeakObjectPtr<UTARibbonColliderComponent>& A, const TWeakObjectPtr<UTARibbonColliderComponent>& B)
	{
		return A->GetPathName().Compare(B->GetPathName(), ESearchCase::CaseSensitive) < 0;
	});
	TSet<FTARibbonCollisionRegistry::FKey> Seen;
	for (const TWeakObjectPtr<UTARibbonColliderComponent>& Entry : Colliders)
	{
		UTARibbonColliderComponent* Component = Entry.Get();
		if (!IsValid(Component->GetOwner())) { continue; }
		const FTARibbonCollisionRegistry::FKey Key{ Component->GetOwner(), Component->GetFName() };
		Seen.Add(Key);
		auto& History = CollisionRegistry->Histories.FindOrAdd(Key);
		Component->RefreshColliderValidation();
		FTARibbonColliderGeometry Geometry;
		FString Error;
		const bool bValid = Component->GetColliderGeometry(Geometry, Error);
		if (!bValid)
		{
			++Snapshot->InvalidColliderCount;
			Snapshot->Warning += FString::Printf(TEXT("%s: %s "), *Component->GetPathName(), *Error);
		}
		if (!Component->bEnabled || !bValid)
		{
			History.bWasEnabled = false;
			History.Trajectory.Samples.Reset();
			continue;
		}
		const bool bRevisionChanged = History.LastComponent.Get() == Component
			? History.MotionRevision != Component->GetMotionHistoryRevision() : Component->GetMotionHistoryRevision() != 0;
		const bool bReset = !History.bWasEnabled || bRevisionChanged;
		UE::TARibbon::RecordColliderPose(History.Trajectory, Geometry, TimeSeconds, TimeSeconds - HistorySeconds, bReset);
		History.MotionRevision = Component->GetMotionHistoryRevision();
		History.LastComponent = Component;
		History.bWasEnabled = true;
		Snapshot->Colliders.Add(History.Trajectory);
	}
	// Defer removal until capture: unregister/register within one reconstruction retains history.
	for (auto It = CollisionRegistry->Histories.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key())) { It.RemoveCurrent(); }
	}
	return Snapshot;
}

void UTARibbonWorldSubsystem::Tick(float DeltaTime)
{
	if (LastDispatchFrame == GFrameCounter || !GetWorld()) { return; }
	LastDispatchFrame = GFrameCounter;
	Components.RemoveAll([this](const TWeakObjectPtr<UTARibbonComponent>& Entry)
	{
		return !Entry.IsValid() || !Entry->IsRegistered() || Entry->GetWorld() != GetWorld();
	});
	double HistorySeconds = 1.0 / 60.0;
	for (const TWeakObjectPtr<UTARibbonComponent>& Entry : Components)
	{
		if (const UTARibbonComponent* Component = Entry.Get())
		{
			const double Window = static_cast<double>(Component->FixedTimeStep) * FMath::Max(1, Component->MaxTicksPerFrame);
			if (FMath::IsFinite(Window)) { HistorySeconds = FMath::Max(HistorySeconds, Window); }
		}
	}
	const auto Snapshot = CaptureColliders(GetWorld()->GetTimeSeconds(), HistorySeconds + FMath::Max(0.0f, DeltaTime));

	TArray<FTARibbonWorldDispatchRequest> Requests;
	Requests.Reserve(Components.Num());
	for (const TWeakObjectPtr<UTARibbonComponent>& Entry : Components)
	{
		if (UTARibbonComponent* Component = Entry.Get())
		{
			if (GetWorld()->WorldType == EWorldType::Editor && !Component->IsEditorPreviewActive()) { continue; }
			Component->ActiveColliderCount = Component->bEnableClothCollision ? Snapshot->Colliders.Num() : 0;
			Component->InvalidColliderCount = Component->bEnableClothCollision ? Snapshot->InvalidColliderCount : 0;
			Component->CollisionWarning = Component->bEnableClothCollision ? Snapshot->Warning : FString();
			if (Component->ActiveColliderCount > UE::TARibbon::MaxColliderCount)
			{
				Component->CollisionWarning = TEXT("超过 64 个启用的布料碰撞体；请减少数量后执行 Reset Simulation。");
				Component->SetRuntimeError(Component->CollisionWarning);
				continue;
			}
			FTARibbonWorldDispatchRequest Request;
			if (Component->BuildWorldDispatch(DeltaTime, Request))
			{
				if (Component->bEnableClothCollision) { Request.CollisionSnapshot = Snapshot; }
				Requests.Add(MoveTemp(Request));
			}
		}
	}
	if (Requests.IsEmpty())
	{
		return;
	}

	ENQUEUE_RENDER_COMMAND(TARibbonWorldFrame)(
		[Requests = MoveTemp(Requests)](FRHICommandListImmediate& RHICmdList) mutable
		{
			FRDGBuilder GraphBuilder(
				RHICmdList,
				RDG_EVENT_NAME("TA.Ribbon.WorldFrame Instances=%d", Requests.Num()));
			for (FTARibbonWorldDispatchRequest& Request : Requests)
			{
				if (!Request.Simulation || !Request.Prepared)
				{
					continue;
				}
				Request.Simulation->PollDiagnostics();

				FString Error;
				if (Request.bCreate)
				{
					FTARibbonGpuCreateRequest CreateRequest;
					CreateRequest.InitialUpload = &Request.Prepared->UploadState;
					if (!Request.Simulation->Create(GraphBuilder, CreateRequest, &Error))
					{
						UE_LOG(LogTARibbon, Error, TEXT("GPU simulation creation failed: %s"), *Error);
						continue;
					}
				}

				FTARibbonGpuFrameRequest FrameRequest;
				FrameRequest.CookedMesh = &Request.Prepared->CookedMesh;
				FrameRequest.Config = &Request.Prepared->Config;
				FrameRequest.UploadState = &Request.Prepared->UploadState;
				FrameRequest.FixedTickCount = Request.FixedTickCount;
				FrameRequest.StartTime = Request.StartTime;
				FrameRequest.bUseSceneWind = Request.bUseSceneWind;
				FrameRequest.SceneWindParameters = Request.SceneWindParameters;
				FrameRequest.SceneWindTimeOffset = Request.SceneWindTimeOffset;
				FrameRequest.CollisionSnapshot = Request.CollisionSnapshot;
				FrameRequest.PrebuiltRenderPlan = &Request.Prepared->RenderPlan;
				FrameRequest.RenderVertexCount = static_cast<uint32>(
					Request.Prepared->UploadState.floats.at("RenderPosition").size());
				if (!Request.Simulation->AddFrame(GraphBuilder, FrameRequest, &Error))
				{
					UE_LOG(LogTARibbon, Error, TEXT("GPU simulation frame failed: %s"), *Error);
					continue;
				}
				if (Request.bReadbackDiagnostics &&
					!Request.Simulation->EnqueueDiagnosticsReadback(GraphBuilder, &Error) && !Error.IsEmpty())
				{
					UE_LOG(LogTARibbon, Warning, TEXT("GPU diagnostics readback was not queued: %s"), *Error);
				}
			}
			GraphBuilder.Execute();
		});
}

TStatId UTARibbonWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTARibbonWorldSubsystem, STATGROUP_Tickables);
}

bool UTARibbonWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE ||
		WorldType == EWorldType::GamePreview || WorldType == EWorldType::Editor;
}

void UTARibbonWorldSubsystem::RegisterRibbon(UTARibbonComponent* Component)
{
	if (IsValid(Component) && Component->GetWorld() == GetWorld())
	{
		Components.AddUnique(Component);
	}
}

void UTARibbonWorldSubsystem::UnregisterRibbon(UTARibbonComponent* Component)
{
	Components.RemoveAll([Component](const TWeakObjectPtr<UTARibbonComponent>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == Component;
	});
}

void UTARibbonWorldSubsystem::RegisterCollider(UTARibbonColliderComponent* Component)
{
	if (IsValid(Component) && Component->GetWorld() == GetWorld()) { Colliders.AddUnique(Component); }
}

void UTARibbonWorldSubsystem::UnregisterCollider(UTARibbonColliderComponent* Component)
{
	Colliders.RemoveAll([Component](const TWeakObjectPtr<UTARibbonColliderComponent>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == Component;
	});
}

void UTARibbonWorldSubsystem::Deinitialize()
{
	const auto Registered = Components;
	for (const auto& Entry : Registered)
	{
		if (UTARibbonComponent* Component = Entry.Get())
		{
			Component->StopEditorPreview();
			Component->UnregisterFromWorldSubsystem();
			Component->DestroyRuntimeRenderer();
			Component->ReleaseGpuSimulation();
			Component->PreparedSimulation.Reset();
		}
	}
	Components.Reset();
	Colliders.Reset();
	CollisionRegistry.Reset();
	Super::Deinitialize();
}
