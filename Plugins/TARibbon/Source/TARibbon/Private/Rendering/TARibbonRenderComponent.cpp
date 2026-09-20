#include "Rendering/TARibbonRenderComponent.h"

#include "Rendering/TARibbonSceneProxy.h"
#include "TARibbonComponent.h"
#include "TARibbonMeshData.h"
#include "TARibbonRuntimePrivate.h"

UTARibbonRenderComponent::UTARibbonRenderComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	Mobility = EComponentMobility::Movable;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	bCanEverAffectNavigation = false;
	bAffectDistanceFieldLighting = false;
	bAffectDynamicIndirectLighting = false;
	ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
}

void UTARibbonRenderComponent::InitializeFromSource(
	UTARibbonComponent* InController,
	UStaticMeshComponent* InSourceComponent)
{
	Controller = InController;
	if (!InSourceComponent)
	{
		SetStaticMesh(nullptr);
		EmptyOverrideMaterials();
		return;
	}

	SetStaticMesh(InSourceComponent->GetStaticMesh());
	EmptyOverrideMaterials();
	for (int32 MaterialIndex = 0; MaterialIndex < InSourceComponent->GetNumMaterials(); ++MaterialIndex)
	{
		SetMaterial(MaterialIndex, InSourceComponent->GetMaterial(MaterialIndex));
	}

	SetWorldTransform(InSourceComponent->GetComponentTransform());
	CastShadow = InSourceComponent->CastShadow;
	bCastDynamicShadow = InSourceComponent->bCastDynamicShadow;
	bCastStaticShadow = false;
	bRenderInMainPass = InSourceComponent->bRenderInMainPass;
	bRenderInDepthPass = InSourceComponent->bRenderInDepthPass;
	bReceivesDecals = InSourceComponent->bReceivesDecals;
	LightingChannels = InSourceComponent->LightingChannels;
	MarkRenderStateDirty();
	UpdateBounds();
}

FPrimitiveSceneProxy* UTARibbonRenderComponent::CreateSceneProxy()
{
	if (!Controller || !Controller->PreparedSimulation || !Controller->GpuSimulation)
	{
		return nullptr;
	}

	const UTARibbonMeshData* Data = Controller->GetRibbonMeshData();
	if (!Data)
	{
		return nullptr;
	}

	return CreateTARibbonSceneProxy(
		this,
		*Data,
		Controller->GpuSimulation->GetRenderState(),
		Controller->SimulationTransform);
}

FBoxSphereBounds UTARibbonRenderComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBoxSphereBounds SourceBounds = Super::CalcBounds(LocalToWorld);
	if (!Controller)
	{
		return SourceBounds;
	}
	return SourceBounds.ExpandBy(FMath::Max(0.0f, Controller->AutomaticReachCm + Controller->BoundsExpansion));
}
