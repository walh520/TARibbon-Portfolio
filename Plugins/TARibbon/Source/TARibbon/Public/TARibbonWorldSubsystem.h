#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TARibbonWorldSubsystem.generated.h"

class UTARibbonComponent;
class UTARibbonColliderComponent;
struct FTARibbonCollisionRegistry;
struct FTARibbonCollisionSnapshot;

/** Central fixed-step scheduler. Every registered ribbon is submitted in one RDG graph per world frame. */
UCLASS()
class TARIBBON_API UTARibbonWorldSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual void Deinitialize() override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	void RegisterRibbon(UTARibbonComponent* Component);
	void UnregisterRibbon(UTARibbonComponent* Component);
	void RegisterCollider(UTARibbonColliderComponent* Component);
	void UnregisterCollider(UTARibbonColliderComponent* Component);

private:
	TSharedPtr<const FTARibbonCollisionSnapshot, ESPMode::ThreadSafe> CaptureColliders(double TimeSeconds, double HistorySeconds);
	TArray<TWeakObjectPtr<UTARibbonComponent>> Components;
	TArray<TWeakObjectPtr<UTARibbonColliderComponent>> Colliders;
	TSharedPtr<FTARibbonCollisionRegistry> CollisionRegistry;
	uint64 LastDispatchFrame = MAX_uint64;
};
