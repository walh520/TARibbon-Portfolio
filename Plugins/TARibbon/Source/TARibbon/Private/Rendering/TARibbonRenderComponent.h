#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "TARibbonRenderComponent.generated.h"

class UTARibbonComponent;

/**
 * Runtime-only render carrier for a TARibbon controller. It is never exposed
 * in the Add Component menu or saved into the Actor; authoring stays on the
 * original StaticMeshComponent.
 */
UCLASS(Transient, NotBlueprintable, NotPlaceable, HideDropdown)
class UTARibbonRenderComponent final : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UTARibbonRenderComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	void InitializeFromSource(UTARibbonComponent* InController, UStaticMeshComponent* InSourceComponent);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UTARibbonComponent> Controller;
};
