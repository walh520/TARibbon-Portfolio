#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialRelevance.h"
#include "PrimitiveSceneProxy.h"
#include "StaticMeshResources.h"
#include "TARibbonVertexFactory.h"

class FMaterialRenderProxy;
class UStaticMeshComponent;
class UTARibbonMeshData;
struct FTARibbonRenderState;

namespace UE::TARibbon::Private
{
class FTARibbonStaticVertexBuffer;
class FTARibbonStaticIndexBuffer;
}

using FTARibbonRenderStatePtr = TSharedPtr<FTARibbonRenderState, ESPMode::ThreadSafe>;

/**
 * Render-thread callback used to turn the simulation's persistent pooled
 * buffers into the SRVs consumed by the vertex factory.  The callback is
 * invoked for every mesh draw, not cached in the proxy, so RDG/RHI resource
 * rotation is visible without CPU readback or a proxy rebuild.
 */
using FTARibbonRenderStateResolveFn = bool (*)(
	const FTARibbonRenderState& RenderState,
	FRHICommandListBase& RHICmdList,
	FTARibbonVertexFactoryDrawData& OutDrawData);

class FTARibbonSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FTARibbonSceneProxy(
		const FPrimitiveSceneProxyDesc& InPrimitiveDesc,
		const UTARibbonMeshData& InMeshData,
		TConstArrayView<const FMaterialRenderProxy*> InMaterials,
		int32 InMaterialIndex,
		const FMaterialRelevance& InMaterialRelevance,
		const FMatrix44f& InWorldToComponentLocal,
		const FMatrix44f& InWorldToComponentRotation,
		FTARibbonRenderStatePtr InRenderState,
		FTARibbonRenderStateResolveFn InResolveRenderState,
		bool bInVelocityRelevance,
		int32 InLODIndex = 0);

	virtual ~FTARibbonSceneProxy() override;

	virtual SIZE_T GetTypeHash() const override;
	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;
	virtual uint32 GetMemoryFootprint() const override;

private:
	static bool ResolveRenderState_RenderThread(
		const FTARibbonRenderState& RenderState,
		FRHICommandListBase& RHICmdList,
		FTARibbonVertexFactoryDrawData& OutDrawData);

	TUniquePtr<UE::TARibbon::Private::FTARibbonStaticVertexBuffer> StaticVertexBuffer;
	TUniquePtr<UE::TARibbon::Private::FTARibbonStaticIndexBuffer> StaticIndexBuffer;
	TArray<const FMaterialRenderProxy*> Materials;
	FMaterialRelevance MaterialRelevance;
	FMatrix44f WorldToComponentLocal = FMatrix44f::Identity;
	FMatrix44f WorldToComponentRotation = FMatrix44f::Identity;
	FTARibbonRenderStatePtr RenderState;
	FTARibbonRenderStateResolveFn ResolveRenderState = nullptr;
	bool bVelocityRelevance = true;
	int32 MaterialIndex = 0;
	int32 RenderVertexCount = 0;
	int32 RenderIndexCount = 0;
	int32 LODIndex = 0;

	mutable FTARibbonVertexFactory VertexFactory;
};

/** Factory called by the private runtime render component. */
FPrimitiveSceneProxy* CreateTARibbonSceneProxy(
	const UStaticMeshComponent* Component,
	const UTARibbonMeshData& MeshData,
	FTARibbonRenderStatePtr RenderState,
	const FTransform& SimulationTransform);
