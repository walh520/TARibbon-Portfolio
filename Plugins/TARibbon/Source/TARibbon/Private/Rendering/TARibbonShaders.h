#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RenderGraphResources.h"
#include "ShaderParameterStruct.h"

/**
 * The parameter block deliberately mirrors RibbonCommon.ush and RibbonKernels.usf.
 * All simulation buffers are structured 16-byte records; the CPU upload layer owns
 * their contents and this file only describes the UE shader binding contract.
 */
BEGIN_SHADER_PARAMETER_STRUCT(FTARibbonShaderParameters, )
	SHADER_PARAMETER(FVector4f, Params0)
	SHADER_PARAMETER(FVector4f, Params1)
	SHADER_PARAMETER(FVector4f, Params2)
	SHADER_PARAMETER(FVector4f, Params3)
	SHADER_PARAMETER(FVector4f, GravityAndTime)
	SHADER_PARAMETER(FVector4f, ArtWindParams)
	SHADER_PARAMETER(FVector4f, PassParams)

	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, State0)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, State1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, VertexRest)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, PinTarget)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, PinPreviousTarget)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, ExternalImpulse)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, SubstepStartPos)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, FreeVelocity)

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, TriIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriRest0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriRest1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriRest2)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriRest3)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriRest4)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, TriArtCoord)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, FaceWind)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, FaceAeroForce)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, PackedVertexFaceOffsets)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, PackedVertexFaceRefs)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ArtWave0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ArtWave1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ArtWave2)

	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, TriangleLambda)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, PackedTriangleColorRefs)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, TriangleColorRanges)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, HingeIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, HingeRest)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, HingeLambda)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, PackedHingeColorRefs)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, HingeColorRanges)

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ColliderMeta)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ColliderGeometry0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ColliderGeometry1)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ColliderMotion0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, ColliderMotion1)

	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, VertexNormal)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, VertexTangent)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, VertexRestNormal)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, RenderBinding)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, RenderBaryOffset)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RenderPosition)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RenderPreviousPosition)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RenderNormal)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RenderTangent)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FUintVector4>, Diagnostics)

	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, SoftPinLambda)
END_SHADER_PARAMETER_STRUCT()

/** Parameters for the independent per-substep SceneWind adapter pass. */
BEGIN_SHADER_PARAMETER_STRUCT(FTARibbonSceneWindShaderParameters, )
	SHADER_PARAMETER(uint32, TriangleCount)
	SHADER_PARAMETER(float, SampleTimeSeconds)
	SHADER_PARAMETER(FVector2f, BoundsCenter)
	SHADER_PARAMETER(FVector2f, BoundsExtent)
	SHADER_PARAMETER(uint32, WindMode)
	SHADER_PARAMETER(FVector2f, BaseDirection)
	SHADER_PARAMETER(float, BaseSpeed)
	SHADER_PARAMETER(float, NoiseStrength)
	SHADER_PARAMETER(float, NoiseScale)
	SHADER_PARAMETER(float, GustStrength)
	SHADER_PARAMETER(float, GustFrequency)
	SHADER_PARAMETER(float, RandomScale)
	SHADER_PARAMETER(float, RandomStrength)
	SHADER_PARAMETER(float, RandomDirectionalBias)
	SHADER_PARAMETER(float, WaveLengthCm)
	SHADER_PARAMETER(float, WaveSpeedCmS)
	SHADER_PARAMETER(float, WaveAmplitude)
	SHADER_PARAMETER(float, CrossWaveLengthCm)
	SHADER_PARAMETER(float, WaveSideStrength)
	SHADER_PARAMETER(float, UniformMicroTurbulenceStrength)
	SHADER_PARAMETER(float, UniformMicroTurbulenceScale)

	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, State0)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector4>, TriIndices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, FaceWind)
END_SHADER_PARAMETER_STRUCT()

class FTARibbonSampleSceneWindCS final : public FGlobalShader
{
public:
	using FParameters = FTARibbonSceneWindShaderParameters;
	DECLARE_GLOBAL_SHADER(FTARibbonSampleSceneWindCS);
	SHADER_USE_PARAMETER_STRUCT(FTARibbonSampleSceneWindCS, FGlobalShader);
};

#define TARIBBON_DECLARE_COMPUTE_SHADER(ShaderClass) \
	class ShaderClass final : public FGlobalShader \
	{ \
	public: \
		using FParameters = FTARibbonShaderParameters; \
		DECLARE_GLOBAL_SHADER(ShaderClass); \
		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader); \
	};

TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonClearDiagnosticsCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonComputeFaceAeroForcesCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonPredictVerticesCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonSolveTriangleColorCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonResetTriangleLambdasCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonResetHingeLambdasCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonResetSoftPinLambdasCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonSolveSoftPinsCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonSolveHingeColorCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonProjectContactsCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonUpdateVelocitiesAndApplyContactFrictionCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonGatherVertexFramesCS)
TARIBBON_DECLARE_COMPUTE_SHADER(FTARibbonBuildRenderVerticesCS)

#undef TARIBBON_DECLARE_COMPUTE_SHADER
