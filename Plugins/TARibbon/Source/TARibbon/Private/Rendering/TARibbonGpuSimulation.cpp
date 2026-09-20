#include "Rendering/TARibbonGpuSimulation.h"

#include "Rendering/TARibbonShaders.h"
#include "TARibbonCollision.h"
#include "UnifiedBuffer.h"

namespace
{
	using taribbon::gpu::Float4;
	using taribbon::gpu::UInt4;

	static const char* const GFloatResourceNames[] =
	{
		"State0", "State1", "VertexRest", "PinTarget", "PinPreviousTarget", "ExternalImpulse",
		"SubstepStartPos", "FreeVelocity", "TriRest0", "TriRest1", "TriRest2", "TriRest3", "TriRest4",
		"TriArtCoord", "FaceWind", "FaceAeroForce", "ArtWave0", "ArtWave1", "ArtWave2", "TriangleLambda",
		"HingeRest", "HingeLambda", "ColliderMeta", "ColliderGeometry0", "ColliderGeometry1", "ColliderMotion0",
		"ColliderMotion1", "VertexNormal", "VertexTangent", "VertexRestNormal", "RenderBaryOffset", "RenderPosition",
		"RenderPreviousPosition", "RenderNormal", "RenderTangent", "SoftPinLambda"
	};

	static const char* const GUIntResourceNames[] =
	{
		"TriIndices", "PackedVertexFaceOffsets", "PackedVertexFaceRefs", "PackedTriangleColorRefs",
		"TriangleColorRanges", "HingeIndices", "PackedHingeColorRefs", "HingeColorRanges", "RenderBinding", "Diagnostics"
	};

	static const char* const GDynamicFloatNames[] =
	{
		"VertexRest", "PinTarget", "PinPreviousTarget", "ExternalImpulse", "ColliderMeta",
		"ColliderGeometry0", "ColliderGeometry1", "ColliderMotion0", "ColliderMotion1"
	};

	static FVector4f ToVector4(const Float4& Value)
	{
		return FVector4f(Value.x, Value.y, Value.z, Value.w);
	}

	static FVector4f ToVector4(const taribbon::gpu::Parameters& Parameters, int32 Index)
	{
		const Float4* Values = &Parameters.Params0;
		return ToVector4(Values[Index]);
	}

	static FName ToName(const char* Name)
	{
		return FName(UTF8_TO_TCHAR(Name));
	}

	static void SetError(FString* OutError, const TCHAR* Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
	}

	static void SetError(FString* OutError, const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
	}

	static bool HasMatchingCount(FRDGBufferRef Buffer, int32 Count)
	{
		return Buffer && Buffer->Desc.NumElements == static_cast<uint32>(FMath::Max(1, Count));
	}

	static bool QueueFloatUpload(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, const std::vector<Float4>& Source, FString* OutError)
	{
		const int32 Count = FMath::Max(1, static_cast<int32>(Source.size()));
		if (!HasMatchingCount(Buffer, Count))
		{
			SetError(OutError, TEXT("TARibbon float upload element count changed after Create"));
			return false;
		}

		TArray<FVector4f> Converted;
		Converted.SetNum(Count);
		for (int32 Index = 0; Index < Converted.Num(); ++Index)
		{
			Converted[Index] = Index < static_cast<int32>(Source.size()) ? ToVector4(Source[Index]) : FVector4f(0.0f);
		}
		GraphBuilder.QueueBufferUpload(Buffer, Converted.GetData(), Converted.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
		return true;
	}

	// QueueBufferUpload executes before the graph. Dynamic wind/pin inputs must
	// instead be copied at this exact point so each SampleExternalInputs stage is
	// observed by the following dispatch rather than only the final sample.
	static bool QueueFloatUploadPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer,
		const std::vector<Float4>& Source, FString* OutError)
	{
		const int32 Count = FMath::Max(1, static_cast<int32>(Source.size()));
		if (!HasMatchingCount(Buffer, Count))
		{
			SetError(OutError, TEXT("TARibbon dynamic input element count changed after Create"));
			return false;
		}

		FRDGScatterUploadBuffer Upload;
		Upload.Init(GraphBuilder, static_cast<uint32>(Count), sizeof(FVector4f), true, TEXT("TARibbon.DynamicInputUpload"));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector4f Value = Index < static_cast<int32>(Source.size()) ? ToVector4(Source[Index]) : FVector4f(0.0f);
			Upload.Add(static_cast<uint32>(Index), &Value);
		}
		Upload.ResourceUploadTo(GraphBuilder, Buffer);
		return true;
	}

	static bool QueueUIntUpload(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, const std::vector<UInt4>& Source, FString* OutError)
	{
		const int32 Count = FMath::Max(1, static_cast<int32>(Source.size()));
		if (!HasMatchingCount(Buffer, Count))
		{
			SetError(OutError, TEXT("TARibbon uint upload element count changed after Create"));
			return false;
		}

		TArray<FUintVector4> Converted;
		Converted.SetNum(Count);
		for (int32 Index = 0; Index < Converted.Num(); ++Index)
		{
			const UInt4 Value = Index < static_cast<int32>(Source.size()) ? Source[Index] : UInt4{};
			Converted[Index] = FUintVector4(Value.x, Value.y, Value.z, Value.w);
		}
		GraphBuilder.QueueBufferUpload(Buffer, Converted.GetData(), Converted.Num() * sizeof(FUintVector4), ERDGInitialDataFlags::None);
		return true;
	}

	static FRDGBufferRef RegisterBuffer(FRDGBuilder& GraphBuilder, const TRefCountPtr<FRDGPooledBuffer>& Buffer)
	{
		return Buffer.IsValid() ? GraphBuilder.RegisterExternalBuffer(Buffer) : nullptr;
	}

	template<typename TShaderClass>
	static void AddDispatch(FRDGBuilder& GraphBuilder, const taribbon::gpu::Command& Command,
		FTARibbonShaderParameters* Parameters)
	{
		TShaderMapRef<TShaderClass> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TARibbon.Dispatch"),
			ERDGPassFlags::Compute,
			Shader,
			Parameters,
			FIntVector(static_cast<int32>(Command.groupsX), 1, 1));
	}
}

struct FTARibbonGpuSimulation::FResources
{
	TMap<FName, TRefCountPtr<FRDGPooledBuffer>> Floats;
	TMap<FName, TRefCountPtr<FRDGPooledBuffer>> UInts;

	TMap<FName, FRDGBufferRef> Register(FRDGBuilder& GraphBuilder) const
	{
		TMap<FName, FRDGBufferRef> Out;
		for (const TPair<FName, TRefCountPtr<FRDGPooledBuffer>>& Pair : Floats)
		{
			Out.Add(Pair.Key, RegisterBuffer(GraphBuilder, Pair.Value));
		}
		for (const TPair<FName, TRefCountPtr<FRDGPooledBuffer>>& Pair : UInts)
		{
			Out.Add(Pair.Key, RegisterBuffer(GraphBuilder, Pair.Value));
		}
		return Out;
	}
};

FTARibbonGpuSimulation::FTARibbonGpuSimulation()
	: Resources(MakeUnique<FResources>())
	, RenderState(MakeShared<FTARibbonRenderState, ESPMode::ThreadSafe>())
	, DiagnosticsReadback(nullptr)
{
}

FTARibbonGpuSimulation::~FTARibbonGpuSimulation()
{
	Release();
}

bool FTARibbonGpuSimulation::CreateResources(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError)
{
	if (Upload.floats.find("State0") == Upload.floats.end() || Upload.floats.find("State1") == Upload.floats.end())
	{
		SetError(OutError, TEXT("TARibbon Upload must contain State0 and State1"));
		return false;
	}

	Resources->Floats.Reset();
	Resources->UInts.Reset();

	for (const char* Name : GFloatResourceNames)
	{
		const auto It = Upload.floats.find(Name);
		const uint32 ElementCount = static_cast<uint32>(FMath::Max<size_t>(1, It == Upload.floats.end() ? 1 : It->second.size()));
		const FString DebugName = FString::Printf(TEXT("TARibbon.%s"), UTF8_TO_TCHAR(Name));
		const FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), ElementCount);
		TRefCountPtr<FRDGPooledBuffer> Buffer = AllocatePooledBuffer(Desc, *DebugName);
		if (!Buffer.IsValid())
		{
			SetError(OutError, FString::Printf(TEXT("Failed to allocate TARibbon float buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
		Resources->Floats.Add(ToName(Name), MoveTemp(Buffer));
	}

	for (const char* Name : GUIntResourceNames)
	{
		const auto It = Upload.uints.find(Name);
		const uint32 ElementCount = static_cast<uint32>(FMath::Max<size_t>(1, It == Upload.uints.end() ? 1 : It->second.size()));
		const FString DebugName = FString::Printf(TEXT("TARibbon.%s"), UTF8_TO_TCHAR(Name));
		const FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), ElementCount);
		TRefCountPtr<FRDGPooledBuffer> Buffer = AllocatePooledBuffer(Desc, *DebugName);
		if (!Buffer.IsValid())
		{
			SetError(OutError, FString::Printf(TEXT("Failed to allocate TARibbon uint buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
		Resources->UInts.Add(ToName(Name), MoveTemp(Buffer));
	}

	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	for (const char* Name : GFloatResourceNames)
	{
		const auto It = Upload.floats.find(Name);
		if (It == Upload.floats.end())
		{
			SetError(OutError, FString::Printf(TEXT("Missing required TARibbon float buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
		if (!QueueFloatUpload(GraphBuilder, Registered.FindRef(ToName(Name)), It->second, OutError))
		{
			return false;
		}
	}
	for (const char* Name : GUIntResourceNames)
	{
		const auto It = Upload.uints.find(Name);
		if (It == Upload.uints.end())
		{
			SetError(OutError, FString::Printf(TEXT("Missing required TARibbon uint buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
		if (!QueueUIntUpload(GraphBuilder, Registered.FindRef(ToName(Name)), It->second, OutError))
		{
			return false;
		}
	}

	RenderState->RenderPosition = Resources->Floats.FindRef(ToName("RenderPosition"));
	RenderState->RenderPreviousPosition = Resources->Floats.FindRef(ToName("RenderPreviousPosition"));
	RenderState->RenderNormal = Resources->Floats.FindRef(ToName("RenderNormal"));
	RenderState->RenderTangent = Resources->Floats.FindRef(ToName("RenderTangent"));
	RenderState->Diagnostics = Resources->UInts.FindRef(ToName("Diagnostics"));
	RenderState->RenderVertexCount = taribbon::gpu::ReadUIntBits(Upload.parameters.PassParams.y);
	RenderState->Revision++;
	return true;
}

bool FTARibbonGpuSimulation::QueueInitialUpload(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError) const
{
	for (const char* Name : GFloatResourceNames)
	{
		if (Upload.floats.find(Name) == Upload.floats.end())
		{
			SetError(OutError, FString::Printf(TEXT("Missing required TARibbon reset float buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
	}
	for (const char* Name : GUIntResourceNames)
	{
		if (Upload.uints.find(Name) == Upload.uints.end())
		{
			SetError(OutError, FString::Printf(TEXT("Missing required TARibbon reset uint buffer %s"), UTF8_TO_TCHAR(Name)));
			return false;
		}
	}

	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	for (const auto& Pair : Upload.floats)
	{
		const FRDGBufferRef Buffer = Registered.FindRef(ToName(Pair.first.c_str()));
		if (Buffer && !QueueFloatUpload(GraphBuilder, Buffer, Pair.second, OutError))
		{
			return false;
		}
	}
	for (const auto& Pair : Upload.uints)
	{
		const FRDGBufferRef Buffer = Registered.FindRef(ToName(Pair.first.c_str()));
		if (Buffer && !QueueUIntUpload(GraphBuilder, Buffer, Pair.second, OutError))
		{
			return false;
		}
	}
	return true;
}

bool FTARibbonGpuSimulation::QueueResetUpload(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload, FString* OutError) const
{
	return QueueInitialUpload(GraphBuilder, Upload, OutError);
}

bool FTARibbonGpuSimulation::QueueDynamicInputs(FRDGBuilder& GraphBuilder, const taribbon::gpu::Upload& Upload,
	bool bIncludeImpulse, FString* OutError) const
{
	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	for (const char* Name : GDynamicFloatNames)
	{
		if (!bIncludeImpulse && FCStringAnsi::Strcmp(Name, "ExternalImpulse") == 0)
		{
			continue;
		}
		const auto It = Upload.floats.find(Name);
		if (It == Upload.floats.end())
		{
			continue;
		}
		if (!QueueFloatUploadPass(GraphBuilder, Registered.FindRef(ToName(Name)), It->second, OutError))
		{
			return false;
		}
	}
	return true;
}

bool FTARibbonGpuSimulation::AddSceneWindPass(
	FRDGBuilder& GraphBuilder,
	const TMap<FName, FRDGBufferRef>& Registered,
	const FSceneWindFieldEvaluationParameters& Wind,
	uint32 TriangleCount,
	double SampleTimeSeconds,
	FString* OutError) const
{
	if (!Wind.IsValid() || TriangleCount == 0 || !FMath::IsFinite(SampleTimeSeconds))
	{
		SetError(OutError, TEXT("TARibbon SceneWind pass received invalid parameters, time, or triangle count"));
		return false;
	}

	const FRDGBufferRef State0 = Registered.FindRef(ToName("State0"));
	const FRDGBufferRef TriIndices = Registered.FindRef(ToName("TriIndices"));
	const FRDGBufferRef FaceWind = Registered.FindRef(ToName("FaceWind"));
	if (!State0 || !TriIndices || !FaceWind)
	{
		SetError(OutError, TEXT("TARibbon SceneWind pass could not resolve State0, TriIndices, or FaceWind"));
		return false;
	}

	FTARibbonSceneWindShaderParameters* Parameters =
		GraphBuilder.AllocParameters<FTARibbonSceneWindShaderParameters>();
	Parameters->TriangleCount = TriangleCount;
	Parameters->SampleTimeSeconds = static_cast<float>(SampleTimeSeconds);
	Parameters->BoundsCenter = Wind.BoundsCenter;
	Parameters->BoundsExtent = Wind.BoundsExtent;
	Parameters->WindMode = Wind.WindMode;
	Parameters->BaseDirection = Wind.BaseDirection;
	Parameters->BaseSpeed = Wind.BaseSpeed;
	Parameters->NoiseStrength = Wind.NoiseStrength;
	Parameters->NoiseScale = Wind.NoiseScale;
	Parameters->GustStrength = Wind.GustStrength;
	Parameters->GustFrequency = Wind.GustFrequency;
	Parameters->RandomScale = Wind.RandomScale;
	Parameters->RandomStrength = Wind.RandomStrength;
	Parameters->RandomDirectionalBias = Wind.RandomDirectionalBias;
	Parameters->WaveLengthCm = Wind.WaveLengthCm;
	Parameters->WaveSpeedCmS = Wind.WaveSpeedCmS;
	Parameters->WaveAmplitude = Wind.WaveAmplitude;
	Parameters->CrossWaveLengthCm = Wind.CrossWaveLengthCm;
	Parameters->WaveSideStrength = Wind.WaveSideStrength;
	Parameters->UniformMicroTurbulenceStrength = Wind.UniformMicroTurbulenceStrength;
	Parameters->UniformMicroTurbulenceScale = Wind.UniformMicroTurbulenceScale;
	Parameters->State0 = GraphBuilder.CreateSRV(State0);
	Parameters->TriIndices = GraphBuilder.CreateSRV(TriIndices);
	Parameters->FaceWind = GraphBuilder.CreateUAV(FaceWind);

	TShaderMapRef<FTARibbonSampleSceneWindCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("TARibbon.SampleSceneWind Triangles=%u Time=%.4f", TriangleCount, SampleTimeSeconds),
		ERDGPassFlags::Compute,
		Shader,
		Parameters,
		FIntVector(static_cast<int32>((TriangleCount + 127u) / 128u), 1, 1));
	return true;
}

static void FillCommonParameters(FTARibbonShaderParameters& Parameters, const taribbon::gpu::Upload& Upload,
	const TMap<FName, FRDGBufferRef>& Resources, FRDGBuilder& GraphBuilder)
{
	Parameters.Params0 = ToVector4(Upload.parameters, 0);
	Parameters.Params1 = ToVector4(Upload.parameters, 1);
	Parameters.Params2 = ToVector4(Upload.parameters, 2);
	Parameters.Params3 = ToVector4(Upload.parameters, 3);
	Parameters.GravityAndTime = ToVector4(Upload.parameters, 4);
	Parameters.ArtWindParams = ToVector4(Upload.parameters, 5);
	Parameters.PassParams = ToVector4(Upload.parameters, 6);

#define TARIBBON_FLOAT_UAV(Name) Parameters.Name = GraphBuilder.CreateUAV(Resources.FindRef(ToName(#Name)))
#define TARIBBON_FLOAT_SRV(Name) Parameters.Name = GraphBuilder.CreateSRV(Resources.FindRef(ToName(#Name)))
#define TARIBBON_UINT_SRV(Name) Parameters.Name = GraphBuilder.CreateSRV(Resources.FindRef(ToName(#Name)))
	TARIBBON_FLOAT_UAV(State0); TARIBBON_FLOAT_UAV(State1); TARIBBON_FLOAT_SRV(VertexRest);
	TARIBBON_FLOAT_SRV(PinTarget); TARIBBON_FLOAT_SRV(PinPreviousTarget); TARIBBON_FLOAT_UAV(ExternalImpulse);
	TARIBBON_FLOAT_UAV(SubstepStartPos); TARIBBON_FLOAT_UAV(FreeVelocity);
	TARIBBON_UINT_SRV(TriIndices); TARIBBON_FLOAT_SRV(TriRest0); TARIBBON_FLOAT_SRV(TriRest1);
	TARIBBON_FLOAT_SRV(TriRest2); TARIBBON_FLOAT_SRV(TriRest3); TARIBBON_FLOAT_SRV(TriRest4);
	TARIBBON_FLOAT_SRV(TriArtCoord); TARIBBON_FLOAT_SRV(FaceWind); TARIBBON_FLOAT_UAV(FaceAeroForce);
	TARIBBON_UINT_SRV(PackedVertexFaceOffsets); TARIBBON_UINT_SRV(PackedVertexFaceRefs);
	TARIBBON_FLOAT_SRV(ArtWave0); TARIBBON_FLOAT_SRV(ArtWave1); TARIBBON_FLOAT_SRV(ArtWave2);
	TARIBBON_FLOAT_UAV(TriangleLambda); TARIBBON_UINT_SRV(PackedTriangleColorRefs); TARIBBON_UINT_SRV(TriangleColorRanges);
	TARIBBON_UINT_SRV(HingeIndices); TARIBBON_FLOAT_SRV(HingeRest); TARIBBON_FLOAT_UAV(HingeLambda);
	TARIBBON_UINT_SRV(PackedHingeColorRefs); TARIBBON_UINT_SRV(HingeColorRanges);
	TARIBBON_FLOAT_SRV(ColliderMeta); TARIBBON_FLOAT_SRV(ColliderGeometry0); TARIBBON_FLOAT_SRV(ColliderGeometry1);
	TARIBBON_FLOAT_SRV(ColliderMotion0); TARIBBON_FLOAT_SRV(ColliderMotion1);
	TARIBBON_FLOAT_UAV(VertexNormal); TARIBBON_FLOAT_UAV(VertexTangent); TARIBBON_FLOAT_SRV(VertexRestNormal);
	TARIBBON_UINT_SRV(RenderBinding); TARIBBON_FLOAT_SRV(RenderBaryOffset);
	TARIBBON_FLOAT_UAV(RenderPosition); TARIBBON_FLOAT_UAV(RenderPreviousPosition);
	TARIBBON_FLOAT_UAV(RenderNormal); TARIBBON_FLOAT_UAV(RenderTangent); Parameters.Diagnostics = GraphBuilder.CreateUAV(Resources.FindRef(ToName("Diagnostics")));
	TARIBBON_FLOAT_UAV(SoftPinLambda);
#undef TARIBBON_FLOAT_UAV
#undef TARIBBON_FLOAT_SRV
#undef TARIBBON_UINT_SRV
}

bool FTARibbonGpuSimulation::AddPlan(FRDGBuilder& GraphBuilder, const std::vector<taribbon::gpu::Command>& Plan,
	taribbon::gpu::Upload& Upload, const FTARibbonGpuFrameRequest::FSampleExternalInputs& Sampler,
	const FTARibbonGpuFrameRequest* FrameRequest, FString* OutError,
	uint32 RenderVertexCountOverride, bool bOverrideRenderVertexCount)
{
	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	bool bFirstSample = true;
	for (const taribbon::gpu::Command& Command : Plan)
	{
		if (!Command.barrierAfter)
		{
			SetError(OutError, TEXT("TARibbon command plan disabled a required resource dependency"));
			return false;
		}

		if (Command.kind == taribbon::gpu::CommandKind::SampleExternalInputs)
		{
			if (Sampler)
			{
				Sampler(Command.sampleTime, Upload);
			}
			if (FrameRequest && !UE::TARibbon::UpdateCollisionUpload(Upload, FrameRequest->CollisionSnapshot.Get(),
				Command.sampleTime + FrameRequest->SceneWindTimeOffset,
				FrameRequest->Config ? FrameRequest->Config->material.contactFriction : 0.0, OutError))
			{
				RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::CollisionInputInvalid), EMemoryOrder::SequentiallyConsistent);
				RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
				return false;
			}
			if (!QueueDynamicInputs(GraphBuilder, Upload, bFirstSample, OutError))
			{
				return false;
			}

			const FRDGBufferRef FaceWind = Registered.FindRef(ToName("FaceWind"));
			if (!FaceWind)
			{
				SetError(OutError, TEXT("TARibbon SampleExternalInputs could not resolve FaceWind"));
				return false;
			}
			const uint32 TriangleCount = taribbon::gpu::ReadUIntBits(Upload.parameters.Params3.y);
			const double SceneWindSampleTime = FrameRequest
				? Command.sampleTime + FrameRequest->SceneWindTimeOffset
				: 0.0;
			const bool bHasValidSceneWind =
				FrameRequest &&
				FrameRequest->bUseSceneWind &&
				FrameRequest->SceneWindParameters.IsValid() &&
				TriangleCount > 0 &&
				FMath::IsFinite(SceneWindSampleTime);
			if (bHasValidSceneWind)
			{
				if (!AddSceneWindPass(
					GraphBuilder,
					Registered,
					FrameRequest->SceneWindParameters,
					TriangleCount,
					SceneWindSampleTime,
					OutError))
				{
					return false;
				}
			}
			else
			{
				// Disabled, unresolved, out-of-world, and otherwise invalid sources never
				// retain a previous substep's velocity.
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FaceWind), 0u);
			}
			bFirstSample = false;
			continue;
		}

		if (Command.groupsX == 0)
		{
			SetError(OutError, TEXT("TARibbon dispatch plan contains a zero group count"));
			return false;
		}

		FTARibbonShaderParameters* Parameters = GraphBuilder.AllocParameters<FTARibbonShaderParameters>();
		FillCommonParameters(*Parameters, Upload, Registered, GraphBuilder);
		if (bOverrideRenderVertexCount)
		{
			Parameters->PassParams.Y = taribbon::gpu::UIntBits(RenderVertexCountOverride);
		}
		Parameters->GravityAndTime.W = static_cast<float>(Command.sampleTime);
		if (Command.entryPoint == "SolveTriangleColor" || Command.entryPoint == "SolveHingeColor")
		{
			Parameters->PassParams.X = taribbon::gpu::UIntBits(Command.color);
		}

#define TARIBBON_ADD_COMMAND(Name, ShaderClass) \
		if (Command.entryPoint == Name) { AddDispatch<ShaderClass>(GraphBuilder, Command, Parameters); continue; }
		TARIBBON_ADD_COMMAND("ClearDiagnostics", FTARibbonClearDiagnosticsCS)
		TARIBBON_ADD_COMMAND("ComputeFaceAeroForces", FTARibbonComputeFaceAeroForcesCS)
		TARIBBON_ADD_COMMAND("PredictVertices", FTARibbonPredictVerticesCS)
		TARIBBON_ADD_COMMAND("SolveTriangleColor", FTARibbonSolveTriangleColorCS)
		TARIBBON_ADD_COMMAND("ResetTriangleLambdas", FTARibbonResetTriangleLambdasCS)
		TARIBBON_ADD_COMMAND("ResetHingeLambdas", FTARibbonResetHingeLambdasCS)
		TARIBBON_ADD_COMMAND("ResetSoftPinLambdas", FTARibbonResetSoftPinLambdasCS)
		TARIBBON_ADD_COMMAND("SolveSoftPins", FTARibbonSolveSoftPinsCS)
		TARIBBON_ADD_COMMAND("SolveHingeColor", FTARibbonSolveHingeColorCS)
		TARIBBON_ADD_COMMAND("ProjectContacts", FTARibbonProjectContactsCS)
		TARIBBON_ADD_COMMAND("UpdateVelocitiesAndApplyContactFriction", FTARibbonUpdateVelocitiesAndApplyContactFrictionCS)
		TARIBBON_ADD_COMMAND("GatherVertexFrames", FTARibbonGatherVertexFramesCS)
		TARIBBON_ADD_COMMAND("BuildRenderVertices", FTARibbonBuildRenderVerticesCS)
#undef TARIBBON_ADD_COMMAND

		SetError(OutError, FString::Printf(TEXT("Unknown TARibbon shader entry point: %s"), UTF8_TO_TCHAR(Command.entryPoint.c_str())));
		return false;
	}
	return true;
}

bool FTARibbonGpuSimulation::AddRenderPlan(FRDGBuilder& GraphBuilder, const std::vector<taribbon::gpu::Command>& Plan,
	taribbon::gpu::Upload& Upload, uint32 RenderVertexCount, FString* OutError)
{
	if (RenderVertexCount != RenderState->RenderVertexCount)
	{
		SetError(OutError, TEXT("TARibbon render vertex count changed after Create"));
		return false;
	}
	TFunction<void(double, taribbon::gpu::Upload&)> NoSampler;
	if (!AddPlan(GraphBuilder, Plan, Upload, NoSampler, nullptr, OutError, RenderVertexCount, true))
	{
		return false;
	}
	return true;
}

void FTARibbonGpuSimulation::MarkRenderOutputsForExternalRead(FRDGBuilder& GraphBuilder) const
{
	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	for (const TCHAR* Name : { TEXT("RenderPosition"), TEXT("RenderPreviousPosition"), TEXT("RenderNormal"), TEXT("RenderTangent") })
	{
		if (const FRDGBufferRef Buffer = Registered.FindRef(FName(Name)))
		{
			GraphBuilder.UseExternalAccessMode(Buffer, ERHIAccess::SRVMask);
		}
	}
}

bool FTARibbonGpuSimulation::Create(FRDGBuilder& GraphBuilder, const FTARibbonGpuCreateRequest& Request, FString* OutError)
{
	RenderState->bReady.Store(false, EMemoryOrder::SequentiallyConsistent);
	RenderState->bFailed.Store(false, EMemoryOrder::SequentiallyConsistent);
	RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::None), EMemoryOrder::SequentiallyConsistent);
	RenderState->bDiagnosticsValid.Store(false, EMemoryOrder::SequentiallyConsistent);
	if (!Request.InitialUpload)
	{
		SetError(OutError, TEXT("TARibbon Create requires an initial upload"));
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::MissingInitialUpload), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
		return false;
	}
	if (!CreateResources(GraphBuilder, *Request.InitialUpload, OutError))
	{
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::ResourceCreationFailed), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
		return false;
	}
	bCreated = true;
	RenderState->bReady.Store(RenderState->IsValid(), EMemoryOrder::SequentiallyConsistent);
	if (!RenderState->IsReady())
	{
		bCreated = false;
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::RenderStateInvalid), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
	}
	return RenderState->IsReady();
}

bool FTARibbonGpuSimulation::Reset(FRDGBuilder& GraphBuilder, const FTARibbonGpuResetRequest& Request, FString* OutError)
{
	RenderState->bReady.Store(false, EMemoryOrder::SequentiallyConsistent);
	RenderState->bDiagnosticsValid.Store(false, EMemoryOrder::SequentiallyConsistent);
	if (!bCreated || !Request.ResetUpload)
	{
		SetError(OutError, TEXT("TARibbon Reset requires an existing simulation and upload"));
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::ResetBeforeCreate), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
		return false;
	}
	if (!QueueResetUpload(GraphBuilder, *Request.ResetUpload, OutError))
	{
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::ResetUploadFailed), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
		return false;
	}
	RenderState->RenderVertexCount = taribbon::gpu::ReadUIntBits(Request.ResetUpload->parameters.PassParams.y);
	RenderState->Revision++;
	RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::None), EMemoryOrder::SequentiallyConsistent);
	RenderState->bFailed.Store(false, EMemoryOrder::SequentiallyConsistent);
	const bool bValid = RenderState->IsValid();
	RenderState->bReady.Store(bValid, EMemoryOrder::SequentiallyConsistent);
	if (!bValid)
	{
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::RenderStateInvalid), EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(true, EMemoryOrder::SequentiallyConsistent);
	}
	return RenderState->IsReady();
}

bool FTARibbonGpuSimulation::AddFrame(FRDGBuilder& GraphBuilder, const FTARibbonGpuFrameRequest& Request, FString* OutError)
{
	if (!bCreated || !RenderState->IsReady() || !Request.UploadState)
	{
		SetError(OutError, TEXT("TARibbon AddFrame requires a created simulation, ready state, and upload"));
		return false;
	}
	if (Request.CookedMesh)
	{
		const uint32 UploadVertexCount = taribbon::gpu::ReadUIntBits(Request.UploadState->parameters.Params3.x);
		const uint32 UploadTriangleCount = taribbon::gpu::ReadUIntBits(Request.UploadState->parameters.Params3.y);
		const uint32 UploadHingeCount = taribbon::gpu::ReadUIntBits(Request.UploadState->parameters.Params3.z);
		if (UploadVertexCount != Request.CookedMesh->restPositions.size()
			|| UploadTriangleCount != Request.CookedMesh->triangles.size()
			|| UploadHingeCount != Request.CookedMesh->hinges.size())
		{
			SetError(OutError, TEXT("TARibbon cooked mesh topology differs from the upload"));
			return false;
		}
	}

	for (uint32 TickIndex = 0; TickIndex < Request.FixedTickCount; ++TickIndex)
	{
		std::vector<taribbon::gpu::Command> GeneratedPlan;
		const std::vector<taribbon::gpu::Command>* Plan = nullptr;
		if (Request.PrebuiltTickPlans && Request.PrebuiltTickPlans->size() > TickIndex)
		{
			Plan = &(*Request.PrebuiltTickPlans)[TickIndex];
		}
		else
		{
			if (!Request.CookedMesh || !Request.Config)
			{
				SetError(OutError, TEXT("TARibbon AddFrame needs cooked mesh/config when no tick plans are supplied"));
				return false;
			}
			GeneratedPlan = taribbon::gpu::BuildTickPlan(*Request.CookedMesh, *Request.Config,
				Request.StartTime + static_cast<double>(TickIndex) * Request.Config->fixedDt);
			Plan = &GeneratedPlan;
		}
		if (!AddPlan(GraphBuilder, *Plan, *Request.UploadState, Request.SampleExternalInputs, &Request, OutError))
		{
			return false;
		}
	}

	if (Request.PrebuiltRenderPlan && Request.RenderVertexCount > 0)
	{
		if (!AddRenderPlan(GraphBuilder, *Request.PrebuiltRenderPlan, *Request.UploadState, Request.RenderVertexCount, OutError))
		{
			return false;
		}
	}
	else if (Request.RenderVertexCount > 0 && Request.CookedMesh)
	{
		const std::vector<taribbon::gpu::Command> RenderPlan = taribbon::gpu::BuildRenderPlan(
			static_cast<uint32>(Request.CookedMesh->restPositions.size()), Request.RenderVertexCount);
		if (!AddRenderPlan(GraphBuilder, RenderPlan, *Request.UploadState, Request.RenderVertexCount, OutError))
		{
			return false;
		}
	}
	else if (Request.RenderVertexCount > 0)
	{
		SetError(OutError, TEXT("TARibbon AddFrame needs a render plan or cooked mesh for rendered vertices"));
		return false;
	}
	MarkRenderOutputsForExternalRead(GraphBuilder);
	return true;
}

bool FTARibbonGpuSimulation::EnqueueDiagnosticsReadback(FRDGBuilder& GraphBuilder, FString* OutError)
{
	if (!bCreated || !RenderState->Diagnostics.IsValid())
	{
		SetError(OutError, TEXT("TARibbon diagnostics buffer is not available"));
		return false;
	}

	const TMap<FName, FRDGBufferRef> Registered = Resources->Register(GraphBuilder);
	const FRDGBufferRef DiagnosticsBuffer = Registered.FindRef(ToName("Diagnostics"));
	if (!DiagnosticsBuffer)
	{
		SetError(OutError, TEXT("TARibbon diagnostics resource could not be registered"));
		return false;
	}
	if (DiagnosticsReadback.IsValid() && !DiagnosticsReadback->IsReady())
	{
		return false;
	}

	if (!DiagnosticsReadback.IsValid())
	{
		DiagnosticsReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("TARibbon.DiagnosticsReadback"));
	}
	AddEnqueueCopyPass(GraphBuilder, DiagnosticsReadback.Get(), DiagnosticsBuffer, sizeof(FUintVector4));
	return true;
}

bool FTARibbonGpuSimulation::PollDiagnostics()
{
	if (!DiagnosticsReadback.IsValid() || !DiagnosticsReadback->IsReady())
	{
		return false;
	}
	const FUintVector4* Values = static_cast<const FUintVector4*>(DiagnosticsReadback->Lock(sizeof(FUintVector4)));
	if (!Values)
	{
		return false;
	}
	RenderState->LatestBadTriangle.Store(Values->X, EMemoryOrder::SequentiallyConsistent);
	RenderState->LatestBadHinge.Store(Values->Y, EMemoryOrder::SequentiallyConsistent);
	RenderState->LatestWindClamped.Store(Values->Z, EMemoryOrder::SequentiallyConsistent);
	RenderState->LatestBadContact.Store(Values->W, EMemoryOrder::SequentiallyConsistent);
	DiagnosticsReadback->Unlock();
	RenderState->bDiagnosticsValid.Store(true, EMemoryOrder::SequentiallyConsistent);
	return true;
}

void FTARibbonGpuSimulation::Release()
{
	bCreated = false;
	DiagnosticsReadback.Reset();
	if (RenderState.IsValid())
	{
		RenderState->bReady.Store(false, EMemoryOrder::SequentiallyConsistent);
		RenderState->bFailed.Store(false, EMemoryOrder::SequentiallyConsistent);
		RenderState->FailureCode.Store(static_cast<uint32>(ETARibbonGpuFailureCode::None), EMemoryOrder::SequentiallyConsistent);
		RenderState->bDiagnosticsValid.Store(false, EMemoryOrder::SequentiallyConsistent);
		RenderState->RenderPosition.SafeRelease();
		RenderState->RenderPreviousPosition.SafeRelease();
		RenderState->RenderNormal.SafeRelease();
		RenderState->RenderTangent.SafeRelease();
		RenderState->Diagnostics.SafeRelease();
		RenderState->RenderVertexCount = 0;
	}
	Resources->Floats.Reset();
	Resources->UInts.Reset();
}
