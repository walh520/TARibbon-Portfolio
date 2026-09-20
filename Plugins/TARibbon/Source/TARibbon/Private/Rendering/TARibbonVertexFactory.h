#pragma once

#include "Components.h"
#include "CoreMinimal.h"
#include "VertexFactory.h"

class FMeshMaterialShader;
class FShaderParameterMap;
struct FMeshBatchElement;
class FMeshDrawSingleShaderBindings;

/**
 * Render-thread data resolved from the simulation state for one mesh draw.
 *
 * The simulation owns the buffers.  The references below deliberately retain
 * the RHI views for the lifetime of the collected mesh batch, while the
 * resolver remains responsible for ensuring that the underlying simulation
 * state is not retired before the draw completes.
 */
struct FTARibbonVertexFactoryDrawData
{
	FShaderResourceViewRHIRef CurrentPosition;
	FShaderResourceViewRHIRef PreviousPosition;
	FShaderResourceViewRHIRef CurrentNormal;
	FShaderResourceViewRHIRef CurrentTangent;

	/** Built once from the captured component transform; input positions are meters. */
	FMatrix44f WorldToComponentLocal = FMatrix44f::Identity;
	FMatrix44f WorldToComponentRotation = FMatrix44f::Identity;

	uint32 VertexIndexOffset = 0;
	uint32 bDeformable = 1;
	uint32 bVelocityEnabled = 1;
};

/** Custom VF for static-mesh attributes plus GPU-owned ribbon vertex frames. */
class FTARibbonVertexFactory final : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FTARibbonVertexFactory);

public:
	using FDataType = FStaticMeshDataType;

	explicit FTARibbonVertexFactory(ERHIFeatureLevel::Type InFeatureLevel)
		: FVertexFactory(InFeatureLevel)
	{
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	void SetData(FRHICommandListBase& RHICmdList, const FDataType& InData);

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override
	{
		FVertexFactory::ReleaseRHI();
	}

	void GetVertexElements(
		ERHIFeatureLevel::Type InFeatureLevel,
		EVertexInputStreamType InputStreamType,
		FDataType& Data,
		FVertexDeclarationElementList& Elements,
		FVertexStreamList& InOutStreams);

	const FDataType& GetData() const
	{
		return Data;
	}

private:
	FDataType Data;
};
