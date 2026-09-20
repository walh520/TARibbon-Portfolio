#include "TARibbonSceneProxy.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshBatch.h"
#include "MeshElementCollector.h"
#include "MeshMaterialShader.h"
#include "PackedNormal.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RenderGraphResources.h"
#include "RHIResourceUtils.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
#include "StaticMeshSceneProxyDesc.h"
#include "TARibbonMeshData.h"
#include "TARibbonGpuSimulation.h"

namespace UE::TARibbon::Private
{
struct FTARibbonStaticVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FPackedNormal TangentX = FPackedNormal(FVector3f(1.0f, 0.0f, 0.0f));
	FPackedNormal TangentZ = FPackedNormal(FVector3f(0.0f, 0.0f, 1.0f));
	FColor Color = FColor::White;
	FVector2f UV0 = FVector2f::ZeroVector;
};

class FTARibbonStaticVertexBuffer final : public FVertexBuffer
{
public:
	TArray<FTARibbonStaticVertex> Vertices;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (!Vertices.IsEmpty())
		{
			VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(
				RHICmdList,
				TEXT("TARibbonStaticVertexBuffer"),
				EBufferUsageFlags::Static,
				MakeConstArrayView(Vertices));
		}
	}
};

class FTARibbonStaticIndexBuffer final : public FIndexBuffer
{
public:
	TArray<uint32> Indices;

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (!Indices.IsEmpty())
		{
			IndexBufferRHI = UE::RHIResourceUtils::CreateIndexBufferFromArray(
				RHICmdList,
				TEXT("TARibbonStaticIndexBuffer"),
				EBufferUsageFlags::Static,
				MakeConstArrayView(Indices));
		}
	}
};

static void BuildStaticMeshVertexData(const FTARibbonStaticVertexBuffer& StaticVertexBuffer, FTARibbonVertexFactory& VertexFactory, FStaticMeshDataType& OutData)
{
	const uint32 Stride = sizeof(FTARibbonStaticVertex);
	OutData.PositionComponent = FVertexStreamComponent(&StaticVertexBuffer, STRUCT_OFFSET(FTARibbonStaticVertex, Position), Stride, VET_Float3);
	OutData.TangentBasisComponents[0] = FVertexStreamComponent(&StaticVertexBuffer, STRUCT_OFFSET(FTARibbonStaticVertex, TangentX), Stride, VET_PackedNormal);
	OutData.TangentBasisComponents[1] = FVertexStreamComponent(&StaticVertexBuffer, STRUCT_OFFSET(FTARibbonStaticVertex, TangentZ), Stride, VET_PackedNormal);
	OutData.ColorComponent = FVertexStreamComponent(&StaticVertexBuffer, STRUCT_OFFSET(FTARibbonStaticVertex, Color), Stride, VET_Color);
	OutData.TextureCoordinates.Add(FVertexStreamComponent(&StaticVertexBuffer, STRUCT_OFFSET(FTARibbonStaticVertex, UV0), Stride, VET_Float2));
	OutData.NumTexCoords = 1;
	OutData.LODLightmapDataIndex = 0;
}
}

FTARibbonSceneProxy::FTARibbonSceneProxy(
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
	int32 InLODIndex)
	: FPrimitiveSceneProxy(InPrimitiveDesc, FName(TEXT("TARibbon")))
	, StaticVertexBuffer(MakeUnique<UE::TARibbon::Private::FTARibbonStaticVertexBuffer>())
	, StaticIndexBuffer(MakeUnique<UE::TARibbon::Private::FTARibbonStaticIndexBuffer>())
	, Materials(InMaterials)
	, MaterialRelevance(InMaterialRelevance)
	, WorldToComponentLocal(InWorldToComponentLocal)
	, WorldToComponentRotation(InWorldToComponentRotation)
	, RenderState(MoveTemp(InRenderState))
	, ResolveRenderState(InResolveRenderState ? InResolveRenderState : &ResolveRenderState_RenderThread)
	, bVelocityRelevance(bInVelocityRelevance)
	, MaterialIndex(InMaterialIndex)
	, RenderVertexCount(InMeshData.RenderBindings.Num())
	, RenderIndexCount(InMeshData.RenderIndices.Num())
	, LODIndex(InLODIndex)
	, VertexFactory(InPrimitiveDesc.FeatureLevel)
{
	// GPU-deformed geometry must not be treated as a static cached mesh.  The
	// conservative component bounds are supplied in InPrimitiveDesc and remain
	// valid for the full simulation reach.
	bHasDeformableMesh = true;
	bAlwaysHasVelocity = bInVelocityRelevance;
	bGoodCandidateForCachedShadowmap = false;

	StaticVertexBuffer->Vertices.Reserve(InMeshData.RenderBindings.Num());
	for (const FTARibbonRenderVertexBinding& Binding : InMeshData.RenderBindings)
	{
		UE::TARibbon::Private::FTARibbonStaticVertex& Vertex = StaticVertexBuffer->Vertices.AddDefaulted_GetRef();
		Vertex.UV0 = Binding.UV0;
	}
	StaticIndexBuffer->Indices.Reserve(InMeshData.RenderIndices.Num());
	for (const int32 Index : InMeshData.RenderIndices)
	{
		StaticIndexBuffer->Indices.Add(static_cast<uint32>(Index));
	}

	ENQUEUE_RENDER_COMMAND(InitTARibbonVertexFactory)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			StaticVertexBuffer->InitResource(RHICmdList);
			StaticIndexBuffer->InitResource(RHICmdList);
			FStaticMeshDataType VertexData;
			UE::TARibbon::Private::BuildStaticMeshVertexData(*StaticVertexBuffer, VertexFactory, VertexData);
			VertexFactory.SetData(RHICmdList, VertexData);
			VertexFactory.InitResource(RHICmdList);
		});
}

FTARibbonSceneProxy::~FTARibbonSceneProxy()
{
	VertexFactory.ReleaseResource();
	StaticVertexBuffer->ReleaseResource();
	StaticIndexBuffer->ReleaseResource();
}

SIZE_T FTARibbonSceneProxy::GetTypeHash() const
{
	static int32 UniqueType;
	return reinterpret_cast<SIZE_T>(&UniqueType);
}

bool FTARibbonSceneProxy::ResolveRenderState_RenderThread(
	const FTARibbonRenderState& InRenderState,
	FRHICommandListBase& RHICmdList,
	FTARibbonVertexFactoryDrawData& OutDrawData)
{
	if (!InRenderState.IsReady() || !InRenderState.IsValid())
	{
		return false;
	}

	// The buffers are pooled RDG resources. Resolve their SRVs at draw time so
	// resource rotation/reallocation is visible without rebuilding the proxy.
	OutDrawData.CurrentPosition = InRenderState.RenderPosition->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
	OutDrawData.PreviousPosition = InRenderState.RenderPreviousPosition->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
	OutDrawData.CurrentNormal = InRenderState.RenderNormal->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
	OutDrawData.CurrentTangent = InRenderState.RenderTangent->GetSRV(RHICmdList, FRHIBufferSRVCreateInfo());
	OutDrawData.bDeformable = 1;
	OutDrawData.bVelocityEnabled = 1;
	return OutDrawData.CurrentPosition.IsValid() && OutDrawData.PreviousPosition.IsValid()
		&& OutDrawData.CurrentNormal.IsValid() && OutDrawData.CurrentTangent.IsValid();
}

void FTARibbonSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	if (!StaticIndexBuffer.IsValid() || RenderIndexCount < 3 || RenderVertexCount < 1 || !RenderState.IsValid() || !VertexFactory.IsInitialized())
	{
		return;
	}

	if (!Materials.IsValidIndex(MaterialIndex) || !Materials[MaterialIndex])
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1u << ViewIndex)) == 0)
		{
			continue;
		}

		FTARibbonVertexFactoryDrawData& DrawData = Collector.AllocateOneFrameResource<FTARibbonVertexFactoryDrawData>();
		if (!ResolveRenderState(*RenderState, Collector.GetRHICommandList(), DrawData))
		{
			continue;
		}
		DrawData.WorldToComponentLocal = WorldToComponentLocal;
		DrawData.WorldToComponentRotation = WorldToComponentRotation;

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &VertexFactory;
		Mesh.MaterialRenderProxy = Materials[MaterialIndex];
		Mesh.LCI = nullptr;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.CastShadow = CastsDynamicShadow() || CastsStaticShadow();
		Mesh.bUseForMaterial = true;
		Mesh.bUseForDepthPass = true;
		Mesh.bUseAsOccluder = true;
		Mesh.bCanApplyViewModeOverrides = true;
		Mesh.bUseWireframeSelectionColoring = true;
		Mesh.LODIndex = static_cast<int8>(LODIndex);

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = StaticIndexBuffer.Get();
		BatchElement.UserData = &DrawData;
		BatchElement.FirstIndex = 0;
		BatchElement.NumPrimitives = RenderIndexCount / 3;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = RenderVertexCount - 1;

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		FPrimitiveUniformShaderParametersBuilder Builder;
		BuildUniformShaderParameters(Builder);
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FTARibbonSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bDynamicRelevance = true;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bVelocityRelevance = bVelocityRelevance && Result.bOpaque && Result.bRenderInMainPass;
	return Result;
}

bool FTARibbonSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

uint32 FTARibbonSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

FPrimitiveSceneProxy* CreateTARibbonSceneProxy(
	const UStaticMeshComponent* Component,
	const UTARibbonMeshData& MeshData,
	FTARibbonRenderStatePtr RenderState,
	const FTransform& SimulationTransform)
{
	if (!Component || !RenderState.IsValid() || !MeshData.IsInternallyConsistent())
	{
		return nullptr;
	}

	if (!SimulationTransform.GetScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		// v1's render contract assumes a unit simulation transform so world-space
		// normals/tangents can be rotated without a normal-matrix scale path.
		return nullptr;
	}

	FStaticMeshSceneProxyDesc ProxyDesc(Component);
	UStaticMesh* StaticMesh = ProxyDesc.GetStaticMesh();
	if (!StaticMesh || StaticMesh->IsNaniteEnabled() || StaticMesh->GetRenderData() == nullptr || StaticMesh->GetRenderData()->LODResources.Num() == 0)
	{
		return nullptr;
	}

	const FStaticMeshLODResources& LODResources = StaticMesh->GetRenderData()->LODResources[0];
	if (LODResources.Sections.Num() != 1 || MeshData.RenderVertexCount <= 0 || MeshData.RenderIndices.Num() < 3 ||
		LODResources.Sections[0].NumTriangles != MeshData.TriangleCount)
	{
		return nullptr;
	}
	const int32 MaterialIndex = LODResources.Sections[0].MaterialIndex;
	if (MaterialIndex < 0 || MaterialIndex >= ProxyDesc.GetNumMaterials())
	{
		return nullptr;
	}
	UMaterialInterface* RibbonMaterial = ProxyDesc.GetMaterial(MaterialIndex);
	if (!RibbonMaterial || !RibbonMaterial->IsTwoSided() ||
		(RibbonMaterial->GetBlendMode() != BLEND_Opaque && RibbonMaterial->GetBlendMode() != BLEND_Masked))
	{
		return nullptr;
	}

	TArray<const FMaterialRenderProxy*> Materials;
	Materials.Reserve(ProxyDesc.GetNumMaterials());
	for (int32 SlotIndex = 0; SlotIndex < ProxyDesc.GetNumMaterials(); ++SlotIndex)
	{
		UMaterialInterface* Material = ProxyDesc.GetMaterial(SlotIndex);
		if (!Material)
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		Materials.Add(Material ? Material->GetRenderProxy() : nullptr);
	}

	if (Materials.IsEmpty())
	{
		return nullptr;
	}

	const FMatrix SimulationLocalToWorld = SimulationTransform.ToMatrixWithScale();
	const FMatrix WorldToComponentLocal = SimulationLocalToWorld.InverseFast();
	const FMatrix WorldToComponentRotation = SimulationTransform.ToMatrixNoScale().InverseFast();
	return new FTARibbonSceneProxy(
		ProxyDesc,
		MeshData,
		Materials,
		MaterialIndex,
		ProxyDesc.GetMaterialRelevance(GMaxRHIShaderPlatform),
		FMatrix44f(WorldToComponentLocal),
		FMatrix44f(WorldToComponentRotation),
		MoveTemp(RenderState),
		nullptr,
		true,
		0);
}
