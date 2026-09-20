#include "TARibbonVertexFactory.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "RenderUtils.h"
#include "ShaderParameterUtils.h"

namespace UE::TARibbon::Private
{
class FTARibbonVertexFactoryShaderParameters final : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FTARibbonVertexFactoryShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		CurrentPosition.Bind(ParameterMap, TEXT("TARibbonCurrentPosition"));
		PreviousPosition.Bind(ParameterMap, TEXT("TARibbonPreviousPosition"));
		CurrentNormal.Bind(ParameterMap, TEXT("TARibbonCurrentNormal"));
		CurrentTangent.Bind(ParameterMap, TEXT("TARibbonCurrentTangent"));
		WorldToComponentLocal.Bind(ParameterMap, TEXT("TARibbonWorldToComponentLocal"));
		WorldToComponentRotation.Bind(ParameterMap, TEXT("TARibbonWorldToComponentRotation"));
		VertexIndexOffset.Bind(ParameterMap, TEXT("TARibbonVertexIndexOffset"));
		Deformable.Bind(ParameterMap, TEXT("TARibbonDeformable"));
		VelocityEnabled.Bind(ParameterMap, TEXT("TARibbonVelocityEnabled"));
	}

	void GetElementShaderBindings(
		const FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const
	{
		const FTARibbonVertexFactoryDrawData* DrawData = static_cast<const FTARibbonVertexFactoryDrawData*>(BatchElement.UserData);
		if (!DrawData)
		{
			return;
		}

		ShaderBindings.Add(CurrentPosition, DrawData->CurrentPosition);
		ShaderBindings.Add(PreviousPosition, DrawData->PreviousPosition);
		ShaderBindings.Add(CurrentNormal, DrawData->CurrentNormal);
		ShaderBindings.Add(CurrentTangent, DrawData->CurrentTangent);
		ShaderBindings.Add(WorldToComponentLocal, DrawData->WorldToComponentLocal);
		ShaderBindings.Add(WorldToComponentRotation, DrawData->WorldToComponentRotation);
		ShaderBindings.Add(VertexIndexOffset, DrawData->VertexIndexOffset);
		ShaderBindings.Add(Deformable, DrawData->bDeformable);
		ShaderBindings.Add(VelocityEnabled, DrawData->bVelocityEnabled);
	}

private:
	LAYOUT_FIELD(FShaderResourceParameter, CurrentPosition);
	LAYOUT_FIELD(FShaderResourceParameter, PreviousPosition);
	LAYOUT_FIELD(FShaderResourceParameter, CurrentNormal);
	LAYOUT_FIELD(FShaderResourceParameter, CurrentTangent);
	LAYOUT_FIELD(FShaderParameter, WorldToComponentLocal);
	LAYOUT_FIELD(FShaderParameter, WorldToComponentRotation);
	LAYOUT_FIELD(FShaderParameter, VertexIndexOffset);
	LAYOUT_FIELD(FShaderParameter, Deformable);
	LAYOUT_FIELD(FShaderParameter, VelocityEnabled);
};

IMPLEMENT_TYPE_LAYOUT(FTARibbonVertexFactoryShaderParameters);
}

bool FTARibbonVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return Parameters.MaterialParameters.bIsUsedWithStaticMesh != 0 ||
		Parameters.MaterialParameters.bIsSpecialEngineMaterial != 0;
}

void FTARibbonVertexFactory::ModifyCompilationEnvironment(
	const FVertexFactoryShaderPermutationParameters& Parameters,
	FShaderCompilerEnvironment& OutEnvironment)
{
	FVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("RAY_TRACING_DYNAMIC_MESH_IN_LOCAL_SPACE"), TEXT("1"));

	const bool bSupportsPrimitiveSceneData = Parameters.VertexFactoryType->SupportsPrimitiveIdStream() &&
		UseGPUScene(Parameters.Platform, GetMaxSupportedFeatureLevel(Parameters.Platform));
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), bSupportsPrimitiveSceneData ? 1 : 0);
}

void FTARibbonVertexFactory::SetData(FRHICommandListBase& RHICmdList, const FDataType& InData)
{
	Data = InData;
	if (IsInitialized())
	{
		UpdateRHI(RHICmdList);
	}
}

void FTARibbonVertexFactory::GetVertexElements(
	ERHIFeatureLevel::Type InFeatureLevel,
	EVertexInputStreamType InputStreamType,
	FDataType& InOutData,
	FVertexDeclarationElementList& Elements,
	FVertexStreamList& InOutStreams)
{
	if (InputStreamType == EVertexInputStreamType::PositionOnly)
	{
		if (InOutData.PositionComponent.VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(InOutData.PositionComponent, 0, InputStreamType));
		}
		AddPrimitiveIdStreamElement(InputStreamType, Elements, 1, 1);
		return;
	}

	if (InputStreamType == EVertexInputStreamType::PositionAndNormalOnly)
	{
		if (InOutData.PositionComponent.VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(InOutData.PositionComponent, 0, InputStreamType));
		}
		if (InOutData.TangentBasisComponents[1].VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(InOutData.TangentBasisComponents[1], 1, InputStreamType));
		}
		AddPrimitiveIdStreamElement(InputStreamType, Elements, 2, 2);
		return;
	}

	check(InputStreamType == EVertexInputStreamType::Default);
	if (InOutData.PositionComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(InOutData.PositionComponent, 0, InOutStreams));
	}

	const uint8 TangentAttributes[2] = {1, 2};
	for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
	{
		if (InOutData.TangentBasisComponents[AxisIndex].VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(InOutData.TangentBasisComponents[AxisIndex], TangentAttributes[AxisIndex], InOutStreams));
		}
	}

	if (InOutData.ColorComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(InOutData.ColorComponent, 3, InOutStreams));
	}
	else
	{
		FVertexStreamComponent NullColorComponent(&GNullColorVertexBuffer, 0, 0, VET_Color, EVertexStreamUsage::ManualFetch);
		Elements.Add(AccessStreamComponent(NullColorComponent, 3, InOutStreams));
	}

	for (int32 CoordinateIndex = 0; CoordinateIndex < InOutData.TextureCoordinates.Num(); ++CoordinateIndex)
	{
		Elements.Add(AccessStreamComponent(InOutData.TextureCoordinates[CoordinateIndex], 4 + CoordinateIndex, InOutStreams));
	}
	if (InOutData.TextureCoordinates.Num() > 0)
	{
		for (int32 CoordinateIndex = InOutData.TextureCoordinates.Num(); CoordinateIndex < MAX_STATIC_TEXCOORDS / 2; ++CoordinateIndex)
		{
			Elements.Add(AccessStreamComponent(InOutData.TextureCoordinates.Last(), 4 + CoordinateIndex, InOutStreams));
		}
	}

	if (InOutData.LightMapCoordinateComponent.VertexBuffer)
	{
		Elements.Add(AccessStreamComponent(InOutData.LightMapCoordinateComponent, 15, InOutStreams));
	}
	else if (InOutData.TextureCoordinates.Num() > 0)
	{
		Elements.Add(AccessStreamComponent(InOutData.TextureCoordinates[0], 15, InOutStreams));
	}
	AddPrimitiveIdStreamElement(InputStreamType, Elements, 13, 13);
}

void FTARibbonVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	FVertexDeclarationElementList Elements;
	GetVertexElements(GetFeatureLevel(), EVertexInputStreamType::PositionOnly, Data, Elements, Streams);
	InitDeclaration(Elements, EVertexInputStreamType::PositionOnly);

	Elements.Reset();
	GetVertexElements(GetFeatureLevel(), EVertexInputStreamType::PositionAndNormalOnly, Data, Elements, Streams);
	InitDeclaration(Elements, EVertexInputStreamType::PositionAndNormalOnly);

	Elements.Reset();
	GetVertexElements(GetFeatureLevel(), EVertexInputStreamType::Default, Data, Elements, Streams);
	InitDeclaration(Elements, EVertexInputStreamType::Default);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FTARibbonVertexFactory, SF_Vertex, UE::TARibbon::Private::FTARibbonVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FTARibbonVertexFactory, "/Plugin/TARibbon/Private/TARibbonVertexFactory.ush",
	EVertexFactoryFlags::UsedWithMaterials |
	EVertexFactoryFlags::SupportsDynamicLighting |
	EVertexFactoryFlags::SupportsPrecisePrevWorldPos |
	EVertexFactoryFlags::SupportsPositionOnly |
	EVertexFactoryFlags::SupportsPrimitiveIdStream);
