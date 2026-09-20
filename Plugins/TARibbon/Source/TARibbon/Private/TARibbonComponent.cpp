#include "TARibbonComponent.h"

#include "TARibbonMeshData.h"
#include "TARibbonModule.h"
#include "TARibbonRuntimePrivate.h"
#include "TARibbonWorldSubsystem.h"
#include "Rendering/TARibbonRenderComponent.h"

#include "SceneWindSubsystem.h"
#include "Wind.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "RenderingThread.h"

#include "Algo/Count.h"

#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

#include <exception>
#include <limits>

namespace UE::TARibbon::Private
{
static taribbon::Vec3 ToRibbonVector(const FVector& Value)
{
	return { Value.X, Value.Y, Value.Z };
}

static FVector ToUnrealVector(const FVector3f& Value)
{
	return FVector(Value.X, Value.Y, Value.Z);
}

static bool IsGameSimulationWorld(const UWorld* World)
{
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE ||
		World->WorldType == EWorldType::GamePreview);
}

struct FReachNode
{
	float Distance = 0.0f;
	int32 Vertex = INDEX_NONE;
	bool operator<(const FReachNode& Other) const { return Distance > Other.Distance; }
};

static float ComputeAutomaticReachCm(const UTARibbonMeshData& Data)
{
	const int32 NumVertices = Data.RestPositions.Num();
	TArray<TArray<TPair<int32, float>>> Adjacency;
	Adjacency.SetNum(NumVertices);
	TSet<uint64> SeenEdges;
	for (int32 Index = 0; Index < Data.TriangleIndices.Num(); Index += 3)
	{
		const int32 Triangle[3] = {
			Data.TriangleIndices[Index], Data.TriangleIndices[Index + 1], Data.TriangleIndices[Index + 2]
		};
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const int32 A = Triangle[Edge];
			const int32 B = Triangle[(Edge + 1) % 3];
			const uint32 MinVertex = static_cast<uint32>(FMath::Min(A, B));
			const uint32 MaxVertex = static_cast<uint32>(FMath::Max(A, B));
			const uint64 Key = (static_cast<uint64>(MinVertex) << 32) | MaxVertex;
			if (!SeenEdges.Contains(Key))
			{
				SeenEdges.Add(Key);
				const float Length = FVector3f::Distance(Data.RestPositions[A], Data.RestPositions[B]);
				Adjacency[A].Emplace(B, Length);
				Adjacency[B].Emplace(A, Length);
			}
		}
	}

	TArray<float> Distance;
	Distance.Init(std::numeric_limits<float>::max(), NumVertices);
	TArray<FReachNode> Heap;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		if (Data.IsPinned(Vertex))
		{
			Distance[Vertex] = 0.0f;
			Heap.HeapPush({ 0.0f, Vertex });
		}
	}
	while (!Heap.IsEmpty())
	{
		FReachNode Node;
		Heap.HeapPop(Node, EAllowShrinking::No);
		if (Node.Distance > Distance[Node.Vertex])
		{
			continue;
		}
		for (const TPair<int32, float>& Edge : Adjacency[Node.Vertex])
		{
			const float Candidate = Node.Distance + Edge.Value;
			if (Candidate < Distance[Edge.Key])
			{
				Distance[Edge.Key] = Candidate;
				Heap.HeapPush({ Candidate, Edge.Key });
			}
		}
	}

	float MaximumReach = 0.0f;
	for (const float VertexDistance : Distance)
	{
		if (FMath::IsFinite(VertexDistance))
		{
			MaximumReach = FMath::Max(MaximumReach, VertexDistance);
		}
	}
	// Compliance can stretch the sheet slightly; keep a deterministic safety margin.
	return MaximumReach * 1.25f;
}

bool ApplyPaintedAeroMasks(
	const UTARibbonMeshData& MeshData,
	taribbon::gpu::Upload& Upload,
	FString* OutError)
{
	const int32 VertexCount = MeshData.RestPositions.Num();
	const int32 TriangleCount = MeshData.TriangleIndices.Num() / 3;
	auto TriangleMasks = Upload.floats.find("TriRest4");
	if (MeshData.TriangleIndices.Num() % 3 != 0 ||
		MeshData.WindResponse.Num() != VertexCount ||
		MeshData.Permeability.Num() != VertexCount ||
		TriangleMasks == Upload.floats.end() ||
		TriangleMasks->second.size() != static_cast<size_t>(TriangleCount))
	{
		if (OutError)
		{
			*OutError = TEXT("Bake v2 aero masks do not match the uploaded physical topology.");
		}
		return false;
	}

	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		const int32 IndexOffset = TriangleIndex * 3;
		const int32 A = MeshData.TriangleIndices[IndexOffset];
		const int32 B = MeshData.TriangleIndices[IndexOffset + 1];
		const int32 C = MeshData.TriangleIndices[IndexOffset + 2];
		if (!MeshData.WindResponse.IsValidIndex(A) || !MeshData.WindResponse.IsValidIndex(B) ||
			!MeshData.WindResponse.IsValidIndex(C))
		{
			if (OutError)
			{
				*OutError = TEXT("Bake v2 aero masks contain an invalid triangle vertex index.");
			}
			return false;
		}

		taribbon::gpu::Float4& TriangleMask = TriangleMasks->second[TriangleIndex];
		TriangleMask.x = (MeshData.WindResponse[A] + MeshData.WindResponse[B] + MeshData.WindResponse[C]) / 3.0f;
		TriangleMask.y = (MeshData.Permeability[A] + MeshData.Permeability[B] + MeshData.Permeability[C]) / 3.0f;
	}
	if (OutError)
	{
		OutError->Reset();
	}
	return true;
}

double ComputeSceneWindTimeOffset(
	double WorldTimeSeconds,
	double RequestStartTime,
	uint32 FixedTickCount,
	double FixedDt)
{
	return WorldTimeSeconds -
		(RequestStartTime + static_cast<double>(FixedTickCount) * FixedDt);
}

struct FClothPresetValues
{
	float ArealDensity;
	float StretchStiffnessU;
	float StretchStiffnessV;
	float ShearStiffness;
	float BendStiffness;
	float DampingRate;
	float Thickness;
	float AirDensity;
	float NormalDrag;
	float TangentDrag;
	float MaxWindSpeed;
	float WindAccelerationClamp;
	float FixedTimeStep;
	int32 Substeps;
	int32 SolverIterations;
	int32 MaxTicksPerFrame;
};

static const FClothPresetValues* FindClothPreset(ETARibbonClothPreset Preset)
{
	// Cotton and silk are the source TARibbon guide presets. Canvas is a deliberately
	// heavier/stiffer portfolio starting point because the guide has no canvas entry.
	static const FClothPresetValues Cotton = {
		0.18f, 2200.0f, 1800.0f, 650.0f, 0.0045f, 1.1f, 0.002f,
		1.225f, 1.18f, 0.14f, 0.0f, 0.0f, 1.0f / 60.0f, 8, 2, 4
	};
	static const FClothPresetValues Silk = {
		0.065f, 780.0f, 620.0f, 155.0f, 0.00055f, 0.55f, 0.002f,
		1.225f, 1.08f, 0.10f, 0.0f, 0.0f, 1.0f / 60.0f, 8, 2, 4
	};
	static const FClothPresetValues Canvas = {
		0.32f, 4200.0f, 3600.0f, 1300.0f, 0.018f, 1.6f, 0.003f,
		1.225f, 1.25f, 0.16f, 0.0f, 0.0f, 1.0f / 60.0f, 8, 2, 4
	};

	switch (Preset)
	{
	case ETARibbonClothPreset::Cotton: return &Cotton;
	case ETARibbonClothPreset::Silk: return &Silk;
	case ETARibbonClothPreset::Canvas: return &Canvas;
	case ETARibbonClothPreset::Custom: return nullptr;
	default: return nullptr;
	}
}

static bool IsClothPresetManagedProperty(FName PropertyName)
{
	return
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, ArealDensity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, StretchStiffnessU) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, StretchStiffnessV) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, ShearStiffness) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, BendStiffness) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, DampingRate) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, Thickness) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, AirDensity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, NormalDrag) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, TangentDrag) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, ContactFriction) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, MaxWindSpeed) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, WindAccelerationClamp) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, FixedTimeStep) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, Substeps) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, SolverIterations) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, MaxTicksPerFrame) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, bEnableArtWind) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, ArtWindWaves);
}
}

UTARibbonComponent::UTARibbonComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	bCanEverAffectNavigation = false;
}

UTARibbonComponent::~UTARibbonComponent()
{
	ReleaseGpuSimulation();
}

void UTARibbonComponent::ApplyClothPresetValues(ETARibbonClothPreset Preset)
{
	const UE::TARibbon::Private::FClothPresetValues* Values = UE::TARibbon::Private::FindClothPreset(Preset);
	if (!Values)
	{
		return;
	}

	ArealDensity = Values->ArealDensity;
	StretchStiffnessU = Values->StretchStiffnessU;
	StretchStiffnessV = Values->StretchStiffnessV;
	ShearStiffness = Values->ShearStiffness;
	BendStiffness = Values->BendStiffness;
	DampingRate = Values->DampingRate;
	Thickness = Values->Thickness;
	AirDensity = Values->AirDensity;
	NormalDrag = Values->NormalDrag;
	TangentDrag = Values->TangentDrag;
	ContactFriction = 0.35f;
	MaxWindSpeed = Values->MaxWindSpeed;
	WindAccelerationClamp = Values->WindAccelerationClamp;
	FixedTimeStep = Values->FixedTimeStep;
	Substeps = Values->Substeps;
	SolverIterations = Values->SolverIterations;
	MaxTicksPerFrame = Values->MaxTicksPerFrame;
	bEnableArtWind = false;
	ArtWindWaves.Reset();
}

void UTARibbonComponent::SetClothPreset(ETARibbonClothPreset NewPreset)
{
#if WITH_EDITOR
	if (!UE::TARibbon::Private::IsGameSimulationWorld(GetWorld()))
	{
		Modify();
	}
#endif
	ClothPreset = NewPreset;
	if (NewPreset == ETARibbonClothPreset::Custom)
	{
		return;
	}

	ApplyClothPresetValues(NewPreset);
	if (CanSimulateInCurrentWorld() && PreparedSimulation)
	{
		SetRuntimeError(TEXT("A TARibbon cloth preset was applied during simulation. Run Reset Simulation to rebuild GPU state."));
	}
	else
	{
		StatusMessage = TEXT("Cloth preset applied. Its cloth, aerodynamic, safety, and solver parameters were updated together.");
	}
	RefreshRuntimeRenderState();
}

void UTARibbonComponent::ApplySelectedClothPreset()
{
	SetClothPreset(ClothPreset);
}

UStaticMeshComponent* UTARibbonComponent::ResolveSourceMeshComponent(FString* OutError) const
{
	if (OutError)
	{
		OutError->Reset();
	}

	// Nesting the controller below a source mesh is the unambiguous authoring path and
	// also allows an Actor to contain unrelated StaticMeshComponents.
	if (UStaticMeshComponent* AttachedMesh = Cast<UStaticMeshComponent>(GetAttachParent()))
	{
		if (AttachedMesh != RuntimeRenderer.Get() && IsValid(AttachedMesh->GetStaticMesh()))
		{
			return AttachedMesh;
		}
	}

	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		if (OutError)
		{
			*OutError = TEXT("TARibbon must belong to an Actor containing a source StaticMeshComponent.");
		}
		return nullptr;
	}

	UStaticMeshComponent* Result = nullptr;
	TInlineComponentArray<UStaticMeshComponent*> MeshComponents;
	Owner->GetComponents(MeshComponents);
	for (UStaticMeshComponent* MeshComponent : MeshComponents)
	{
		if (!MeshComponent || MeshComponent == this || MeshComponent == RuntimeRenderer.Get() ||
			!IsValid(MeshComponent->GetStaticMesh()))
		{
			continue;
		}
		if (Result)
		{
			if (OutError)
			{
				*OutError = TEXT("The Actor contains multiple StaticMeshComponents. Attach TARibbon below the intended source mesh component to select it.");
			}
			return nullptr;
		}
		Result = MeshComponent;
	}

	if (!Result && OutError)
	{
		*OutError = TEXT("No source StaticMeshComponent was found. TARibbon is logic-only; add it to an Actor that already owns the cloth mesh.");
	}
	return Result;
}

UStaticMesh* UTARibbonComponent::GetSourceStaticMesh() const
{
	if (UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent())
	{
		return SourceComponent->GetStaticMesh();
	}
	return nullptr;
}

const UTARibbonMeshData* UTARibbonComponent::GetRibbonMeshData() const
{
	const UStaticMesh* Mesh = GetSourceStaticMesh();
	return Mesh ? Cast<UTARibbonMeshData>(
		const_cast<UStaticMesh*>(Mesh)->GetAssetUserDataOfClass(UTARibbonMeshData::StaticClass())) : nullptr;
}

AWind* UTARibbonComponent::ResolveSceneWindActor(FString* OutWarning) const
{
	if (OutWarning)
	{
		OutWarning->Reset();
	}
	if (WindSourceMode == ETARibbonWindSourceMode::Disabled)
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		if (OutWarning)
		{
			*OutWarning = TEXT("TARibbon cannot resolve SceneWind without a valid World.");
		}
		return nullptr;
	}

	if (WindSourceMode == ETARibbonWindSourceMode::SpecificSceneWind)
	{
		if (!IsValid(SpecificWindActor))
		{
			if (OutWarning)
			{
				*OutWarning = TEXT("Specific SceneWind is selected but no valid Wind Actor is assigned.");
			}
			return nullptr;
		}
		if (SpecificWindActor->GetWorld() != World)
		{
			if (OutWarning)
			{
				*OutWarning = TEXT("The specified Wind Actor belongs to a different World; physical wind is zero.");
			}
			return nullptr;
		}
		return SpecificWindActor;
	}

	if (USceneWindSubsystem* SceneWindSubsystem = World->GetSubsystem<USceneWindSubsystem>())
	{
		if (AWind* ActiveWind = SceneWindSubsystem->GetActiveWind())
		{
			return ActiveWind;
		}
	}
	if (OutWarning)
	{
		*OutWarning = TEXT("No active SceneWind Actor is registered; physical wind is zero.");
	}
	return nullptr;
}

bool UTARibbonComponent::HasEffectiveArtWind() const
{
	if (!bEnableArtWind)
	{
		return false;
	}
	return ArtWindWaves.ContainsByPredicate([](const FTARibbonArtWindWave& Wave)
	{
		return FMath::IsFinite(Wave.Amplitude) && Wave.Amplitude > UE_SMALL_NUMBER;
	});
}

uint32 UTARibbonComponent::ComputeResetRequiredConfigurationHash() const
{
	uint32 Hash = 0;
	auto AddHash = [&Hash](const auto& Value)
	{
		Hash = HashCombineFast(Hash, GetTypeHash(Value));
	};
	AddHash(ArealDensity);
	AddHash(StretchStiffnessU);
	AddHash(StretchStiffnessV);
	AddHash(ShearStiffness);
	AddHash(BendStiffness);
	AddHash(DampingRate);
	AddHash(Thickness);
	AddHash(AirDensity);
	AddHash(NormalDrag);
	AddHash(TangentDrag);
	AddHash(ContactFriction);
	AddHash(MaxWindSpeed);
	AddHash(WindAccelerationClamp);
	AddHash(FixedTimeStep);
	AddHash(Substeps);
	AddHash(SolverIterations);
	AddHash(MaxTicksPerFrame);
	AddHash(bEnableArtWind);
	AddHash(ArtWindWaves.Num());
	for (const FTARibbonArtWindWave& Wave : ArtWindWaves)
	{
		AddHash(Wave.VelocityDirection);
		AddHash(Wave.MaterialDirection);
		AddHash(Wave.Amplitude);
		AddHash(Wave.FrequencyHz);
		AddHash(Wave.Wavelength);
		AddHash(Wave.PhaseSeed);
	}
	return Hash;
}

bool UTARibbonComponent::BuildSceneWindSnapshot(
	FSceneWindFieldEvaluationParameters& OutParameters,
	FString& OutWarning)
{
	OutParameters = FSceneWindFieldEvaluationParameters();
	AWind* ResolvedWindActor = ResolveSceneWindActor(&OutWarning);
	bool bParametersValid = false;
	if (IsValid(ResolvedWindActor))
	{
		OutParameters = ResolvedWindActor->GetWindFieldEvaluationParameters();
		bParametersValid = OutParameters.IsValid();
	}
	if (IsValid(ResolvedWindActor) && !bParametersValid && OutWarning.IsEmpty())
	{
		OutWarning = TEXT("The resolved SceneWind Actor produced an invalid parameter snapshot; physical wind is zero.");
	}
	RefreshInputDiagnostics(ResolvedWindActor, bParametersValid, OutWarning);
	return bParametersValid;
}

void UTARibbonComponent::RefreshInputDiagnostics(
	AWind* ResolvedWindActor,
	bool bEvaluationParametersValid,
	const FString& Warning)
{
	bSceneWindSourceValid = IsValid(ResolvedWindActor) && bEvaluationParametersValid;
	ResolvedWindSourceName = IsValid(ResolvedWindActor) ? ResolvedWindActor->GetPathName() : TEXT("None");
	ResolvedSceneWindMode = IsValid(ResolvedWindActor)
		? UEnum::GetDisplayValueAsText(ResolvedWindActor->GetWindMode()).ToString()
		: (WindSourceMode == ETARibbonWindSourceMode::Disabled ? TEXT("Disabled") : TEXT("Unavailable"));
	SceneWindWarning = Warning;

	bInitialPoseSymmetryWarning =
		MaxRestDihedralDegrees < 0.1f &&
		MaxInitialNormalGravityDot < 0.01f &&
		!bSceneWindSourceValid &&
		!HasEffectiveArtWind();
	InitialPoseWarning = bInitialPoseSymmetryWarning
		? TEXT("The Rest Mesh is nearly flat, gravity is tangent to every initial face, and no wind is active. The exact symmetric state may remain planar; author a slight Rest curvature or rotate the initial pose.")
		: FString();
}

bool UTARibbonComponent::CheckSetup(FString& OutError, bool bCheckFreshness) const
{
	OutError.Reset();
	UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent(&OutError);
	if (!SourceComponent)
	{
		return false;
	}
	const UStaticMesh* Mesh = SourceComponent->GetStaticMesh();
	if (!IsValid(Mesh))
	{
		OutError = TEXT("Assign a Static Mesh to the source component before validating TARibbon.");
		return false;
	}
	if (Mesh->GetNaniteSettings().bEnabled)
	{
		OutError = TEXT("TARibbon v1.2 requires Nanite to be disabled.");
		return false;
	}
	if (Mesh->GetStaticMaterials().Num() != 1)
	{
		OutError = TEXT("TARibbon v1.2 requires exactly one material slot/section.");
		return false;
	}
	const UTARibbonMeshData* Data = GetRibbonMeshData();
	if (!Data)
	{
		OutError = TEXT("The Static Mesh has no TARibbon LOD0 bake. Run Bake Ribbon Data first.");
		return false;
	}
	if (Data->BakeVersion != UTARibbonMeshData::CurrentBakeVersion)
	{
		OutError = FString::Printf(
			TEXT("TARibbon Bake v%d is obsolete; re-run Bake Ribbon Data to create Bake v%d."),
			Data->BakeVersion,
			UTARibbonMeshData::CurrentBakeVersion);
		return false;
	}
	if (!Data->IsInternallyConsistent())
	{
		OutError = TEXT("The Static Mesh TARibbon Bake v2 data is invalid. Re-run Bake Ribbon Data after fixing the reported asset contract.");
		return false;
	}
#if WITH_EDITOR
	if (bCheckFreshness)
	{
		FText FreshnessError;
		if (!Data->IsTopologyCurrent(Mesh, FreshnessError))
		{
			OutError = FreshnessError.ToString();
			return false;
		}
	}
#endif
	if (!SourceComponent->GetComponentScale().Equals(FVector::OneVector, 1.0e-4))
	{
		OutError = TEXT("Apply the mesh transform in Blender and keep the source cloth component scale at (1,1,1).");
		return false;
	}
	if (SourceComponent->Mobility != EComponentMobility::Movable)
	{
		OutError = TEXT("The source cloth StaticMeshComponent mobility must be Movable.");
		return false;
	}
	if (!SourceComponent->CastShadow || !SourceComponent->bCastDynamicShadow)
	{
		OutError = TEXT("Enable Cast Shadow and Cast Dynamic Shadow on the source cloth mesh for TARibbon VSM output.");
		return false;
	}
	UMaterialInterface* Material = SourceComponent->GetMaterial(0);
	if (!Material)
	{
		OutError = TEXT("Assign one two-sided Opaque or Masked surface material.");
		return false;
	}
	const EBlendMode BlendMode = Material->GetBlendMode();
	if ((BlendMode != BLEND_Opaque && BlendMode != BLEND_Masked) || !Material->IsTwoSided())
	{
		OutError = TEXT("TARibbon v1.2 material must be two-sided and Opaque or Masked.");
		return false;
	}
	if (!FMath::IsFinite(ArealDensity) || ArealDensity <= 0.0f ||
		!FMath::IsFinite(StretchStiffnessU) || StretchStiffnessU < 0.0f ||
		!FMath::IsFinite(StretchStiffnessV) || StretchStiffnessV < 0.0f ||
		!FMath::IsFinite(ShearStiffness) || ShearStiffness < 0.0f ||
		!FMath::IsFinite(BendStiffness) || BendStiffness < 0.0f ||
		!FMath::IsFinite(DampingRate) || DampingRate < 0.0f ||
		!FMath::IsFinite(Thickness) || Thickness <= 0.0f ||
		!FMath::IsFinite(AirDensity) || AirDensity < 0.0f ||
		!FMath::IsFinite(NormalDrag) || NormalDrag < 0.0f ||
		!FMath::IsFinite(TangentDrag) || TangentDrag < 0.0f ||
		!FMath::IsFinite(ContactFriction) || ContactFriction < 0.0f ||
		!FMath::IsFinite(MaxWindSpeed) || MaxWindSpeed < 0.0f ||
		!FMath::IsFinite(WindAccelerationClamp) || WindAccelerationClamp < 0.0f ||
		!FMath::IsFinite(FixedTimeStep) || FixedTimeStep <= 0.0f ||
		Substeps < 1 || SolverIterations < 1 || MaxTicksPerFrame < 1)
	{
		OutError = TEXT("One or more TARibbon cloth/solver parameters are outside their valid finite range.");
		return false;
	}
	if (ArtWindWaves.Num() > 4)
	{
		OutError = TEXT("TARibbon supports at most four coherent art-wind waves.");
		return false;
	}
	for (int32 WaveIndex = 0; WaveIndex < ArtWindWaves.Num(); ++WaveIndex)
	{
		const FTARibbonArtWindWave& Wave = ArtWindWaves[WaveIndex];
		const bool bDirectionsValid =
			FMath::IsFinite(Wave.VelocityDirection.X) &&
			FMath::IsFinite(Wave.VelocityDirection.Y) &&
			FMath::IsFinite(Wave.VelocityDirection.Z) &&
			FMath::IsFinite(Wave.MaterialDirection.X) &&
			FMath::IsFinite(Wave.MaterialDirection.Y) &&
			!Wave.VelocityDirection.IsNearlyZero() && !Wave.MaterialDirection.IsNearlyZero();
		if (!bDirectionsValid || !FMath::IsFinite(Wave.Amplitude) || Wave.Amplitude < 0.0f ||
			!FMath::IsFinite(Wave.FrequencyHz) || Wave.FrequencyHz < 0.0f ||
			!FMath::IsFinite(Wave.Wavelength) || Wave.Wavelength <= 0.0f ||
			!FMath::IsFinite(Wave.PhaseSeed))
		{
			OutError = FString::Printf(TEXT("Art-wind wave %d contains an invalid direction or non-finite parameter."), WaveIndex);
			return false;
		}
	}
	return true;
}

bool UTARibbonComponent::PrepareSimulation(FString& OutError)
{
	using namespace UE::TARibbon::Private;
	if (!CheckSetup(OutError, true))
	{
		return false;
	}
	const UTARibbonMeshData* Data = GetRibbonMeshData();
	check(Data);

	TSharedPtr<FTARibbonPreparedSimulation, ESPMode::ThreadSafe> NewPrepared =
		MakeShared<FTARibbonPreparedSimulation, ESPMode::ThreadSafe>();
	UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent();
	check(SourceComponent);
	const FTransform InitialTransform = SourceComponent->GetComponentTransform();
	taribbon::CookInput CookInput;
	CookInput.arealDensity = ArealDensity;
	CookInput.restPositions.reserve(Data->RestPositions.Num());
	CookInput.materialCoordinates.reserve(Data->RestPositions.Num());
	for (const FVector3f& LocalPositionCm : Data->RestPositions)
	{
		const FVector WorldPositionMeters = InitialTransform.TransformPosition(ToUnrealVector(LocalPositionCm)) * 0.01;
		CookInput.restPositions.push_back(ToRibbonVector(WorldPositionMeters));
		CookInput.materialCoordinates.push_back({ LocalPositionCm.X * 0.01, LocalPositionCm.Y * 0.01 });
	}
	CookInput.triangles.reserve(Data->TriangleIndices.Num() / 3);
	for (int32 Index = 0; Index < Data->TriangleIndices.Num(); Index += 3)
	{
		CookInput.triangles.push_back({
			static_cast<uint32>(Data->TriangleIndices[Index]),
			static_cast<uint32>(Data->TriangleIndices[Index + 1]),
			static_cast<uint32>(Data->TriangleIndices[Index + 2])
		});
	}
	const FVector FiberWorld = InitialTransform.TransformVectorNoScale(FVector::XAxisVector).GetSafeNormal();
	CookInput.fiberDirections.assign(CookInput.triangles.size(), ToRibbonVector(FiberWorld));

	taribbon::CookDiagnostics CookDiagnostics;
	std::string CookError;
	if (!taribbon::CookMesh(CookInput, NewPrepared->CookedMesh, &CookDiagnostics, &CookError))
	{
		OutError = FString::Printf(TEXT("TARibbon CookMesh failed: %s"), UTF8_TO_TCHAR(CookError.c_str()));
		return false;
	}
	FallbackFiberTriangleCount = static_cast<int32>(CookDiagnostics.fallbackFiberDirections);
	MaxRestDihedralDegrees = 0.0f;
	for (const taribbon::HingeRest& Hinge : NewPrepared->CookedMesh.hinges)
	{
		MaxRestDihedralDegrees = FMath::Max(
			MaxRestDihedralDegrees,
			FMath::RadiansToDegrees(static_cast<float>(FMath::Abs(Hinge.restAngle))));
	}
	MaxInitialNormalGravityDot = 0.0f;
	const taribbon::Vec3 GravityDirection = taribbon::Normalize({ 0.0, 0.0, -9.81 });
	for (const taribbon::TriangleRest& Triangle : NewPrepared->CookedMesh.triangles)
	{
		MaxInitialNormalGravityDot = FMath::Max(
			MaxInitialNormalGravityDot,
			static_cast<float>(FMath::Abs(taribbon::Dot(Triangle.restNormal, GravityDirection))));
	}

	NewPrepared->Config.fixedDt = FixedTimeStep;
	NewPrepared->Config.substeps = static_cast<uint32>(Substeps);
	NewPrepared->Config.iterations = static_cast<uint32>(SolverIterations);
	NewPrepared->Config.maxTicksPerAdvance = static_cast<uint32>(MaxTicksPerFrame);
	NewPrepared->Config.gravity = { 0.0, 0.0, -9.81 };
	NewPrepared->Config.material.ku = StretchStiffnessU;
	NewPrepared->Config.material.kv = StretchStiffnessV;
	NewPrepared->Config.material.ks = ShearStiffness;
	NewPrepared->Config.material.bendD = BendStiffness;
	NewPrepared->Config.material.dampingRate = DampingRate;
	NewPrepared->Config.material.thickness = Thickness;
	NewPrepared->Config.material.airDensity = AirDensity;
	NewPrepared->Config.material.normalDrag = NormalDrag;
	NewPrepared->Config.material.tangentDrag = TangentDrag;
	NewPrepared->Config.material.windAccelerationClamp = WindAccelerationClamp;
	NewPrepared->Config.material.contactFriction = ContactFriction;
	NewPrepared->Config.artWind.waves.clear();
	if (bEnableArtWind)
	{
		NewPrepared->Config.artWind.waves.reserve(ArtWindWaves.Num());
		for (const FTARibbonArtWindWave& SourceWave : ArtWindWaves)
		{
			taribbon::ArtWindWave Wave;
			Wave.velocityDirection = ToRibbonVector(SourceWave.VelocityDirection);
			Wave.materialDirection = { SourceWave.MaterialDirection.X, SourceWave.MaterialDirection.Y };
			Wave.amplitude = SourceWave.Amplitude;
			Wave.frequencyHz = SourceWave.FrequencyHz;
			Wave.wavelength = SourceWave.Wavelength;
			Wave.phaseSeed = SourceWave.PhaseSeed;
			NewPrepared->Config.artWind.waves.push_back(Wave);
		}
	}

	std::vector<taribbon::Pin> Pins;
	Pins.reserve(Data->PinnedVertices.Num());
	for (int32 Vertex = 0; Vertex < Data->PinnedVertices.Num(); ++Vertex)
	{
		if (Data->IsPinned(Vertex))
		{
			taribbon::Pin Pin;
			Pin.vertex = static_cast<uint32>(Vertex);
			Pin.targetBegin = NewPrepared->CookedMesh.restPositions[Vertex];
			Pin.targetEnd = Pin.targetBegin;
			Pins.push_back(Pin);
		}
	}

	std::vector<taribbon::RenderBinding> RenderBindings;
	RenderBindings.reserve(Data->RenderBindings.Num());
	for (const FTARibbonRenderVertexBinding& Source : Data->RenderBindings)
	{
		taribbon::RenderBinding Binding;
		Binding.triangle = static_cast<uint32>(Source.PhysicalTriangleIndex);
		Binding.barycentric = { Source.Barycentric.X, Source.Barycentric.Y, Source.Barycentric.Z };
		Binding.normalOffset = Source.NormalOffsetMeters;
		Binding.tangentSign = Source.TangentSign;
		RenderBindings.push_back(Binding);
	}

	try
	{
		NewPrepared->UploadState = taribbon::gpu::BuildInitialUpload(
			NewPrepared->CookedMesh, NewPrepared->Config, RenderBindings, Pins, {}, {});
		// Reserve collision capacity even when starting in an empty World.
		if (!UE::TARibbon::UpdateCollisionUpload(NewPrepared->UploadState, nullptr, 0.0, ContactFriction, &OutError))
		{
			return false;
		}
		if (!ApplyPaintedAeroMasks(*Data, NewPrepared->UploadState, &OutError))
		{
			return false;
		}
		for (taribbon::gpu::Float4& FaceWind : NewPrepared->UploadState.floats.at("FaceWind"))
		{
			FaceWind = {};
		}
		NewPrepared->UploadState.parameters.Params1.y = MaxWindSpeed;
		NewPrepared->RenderPlan = taribbon::gpu::BuildRenderPlan(
			static_cast<uint32>(NewPrepared->CookedMesh.restPositions.size()),
			static_cast<uint32>(RenderBindings.size()));
	}
	catch (const std::exception& Exception)
	{
		OutError = FString::Printf(TEXT("TARibbon GPU upload preparation failed: %s"), UTF8_TO_TCHAR(Exception.what()));
		return false;
	}

	ReleaseGpuSimulation();
	PreparedSimulation = MoveTemp(NewPrepared);
	GpuSimulation = MakeShared<FTARibbonGpuSimulation, ESPMode::ThreadSafe>();
	SimulationTransform = InitialTransform;
	SimulationSourceMesh = SourceComponent->GetStaticMesh();
	SimulationSourceMaterial = SourceComponent->GetMaterial(0);
	SimulationConfigurationHash = ComputeResetRequiredConfigurationHash();
	SimulationBakeSignature = Data->TopologySignature;
	AccumulatedSeconds = 0.0;
	SimulatedSeconds = 0.0;
	PendingManualTicks = 0;
	bGpuCreatePending = true;
	AutomaticReachCm = ComputeAutomaticReachCm(*Data);

	SimulationVertexCount = static_cast<int32>(PreparedSimulation->CookedMesh.restPositions.size());
	SimulationTriangleCount = static_cast<int32>(PreparedSimulation->CookedMesh.triangles.size());
	SimulationHingeCount = static_cast<int32>(PreparedSimulation->CookedMesh.hinges.size());
	TriangleColorCount = static_cast<int32>(PreparedSimulation->CookedMesh.triangleColorCount);
	HingeColorCount = static_cast<int32>(PreparedSimulation->CookedMesh.hingeColorCount);
	FixedPointCount = static_cast<int32>(Pins.size());
	GpuBadTriangles = GpuBadHinges = GpuWindClamps = GpuBadContacts = 0;
	FSceneWindFieldEvaluationParameters InitialWindParameters;
	FString InitialWindWarning;
	BuildSceneWindSnapshot(InitialWindParameters, InitialWindWarning);
	StatusMessage = TEXT("Valid LOD0 bake; GPU initialization queued.");
	SimulationStatus = (bRuntimeStarted && !bPaused) ? ETARibbonSimulationStatus::Ready : ETARibbonSimulationStatus::Paused;
	OutError.Reset();
	return true;
}

void UTARibbonComponent::BakeRibbonData()
{
#if WITH_EDITOR
	FString SourceError;
	UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent(&SourceError);
	if (!SourceComponent)
	{
		SetRuntimeError(SourceError);
		UE_LOG(LogTARibbon, Error, TEXT("%s: %s"), *GetPathName(), *StatusMessage);
		return;
	}

	UStaticMesh* Mesh = SourceComponent->GetStaticMesh();
	FText Error;
	if (!FTARibbonMeshBaker::BakeLOD0(Mesh, Error))
	{
		SetRuntimeError(Error.ToString());
		UE_LOG(LogTARibbon, Error, TEXT("%s: %s"), *GetPathName(), *StatusMessage);
		return;
	}
	Mesh->PostEditChange();
	StatusMessage = TEXT("TARibbon Bake v2 LOD0 data was written successfully. Validate or enter PIE to initialize the GPU simulation.");
	SimulationStatus = ETARibbonSimulationStatus::Ready;
	UE_LOG(LogTARibbon, Display, TEXT("%s: %s"), *GetPathName(), *StatusMessage);
	SourceComponent->MarkRenderStateDirty();
	SourceComponent->UpdateBounds();
	RefreshRuntimeRenderState();
#else
	SetRuntimeError(TEXT("Bake Ribbon Data is editor-only."));
#endif
}

void UTARibbonComponent::ValidateRibbonSetup()
{
	FString Error;
	if (CheckSetup(Error, true))
	{
		const UTARibbonMeshData* Data = GetRibbonMeshData();
		SimulationVertexCount = Data ? Data->SimulationVertexCount : 0;
		SimulationTriangleCount = Data ? Data->TriangleCount : 0;
		FixedPointCount = Data ? Algo::CountIf(Data->PinnedVertices, [](uint8 Value) { return Value != 0; }) : 0;
		SimulationStatus = ETARibbonSimulationStatus::Ready;
		StatusMessage = TEXT("TARibbon setup is valid. Runtime uses LOD0, one section, static transform, GPU XPBD, and VSM dynamic shadows.");
		UE_LOG(LogTARibbon, Display, TEXT("%s: %s"), *GetPathName(), *StatusMessage);
	}
	else
	{
		SetRuntimeError(Error);
		UE_LOG(LogTARibbon, Error, TEXT("%s: %s"), *GetPathName(), *StatusMessage);
	}
}

bool UTARibbonComponent::CanSimulateInCurrentWorld() const
{
	return UE::TARibbon::Private::IsGameSimulationWorld(GetWorld()) ||
		(GetWorld() && GetWorld()->WorldType == EWorldType::Editor && bEditorPreviewRequested && !bEditorPreviewSuspended);
}

#if WITH_EDITOR
FTARibbonEditorPreviewRequestChanged& UTARibbonComponent::OnEditorPreviewRequestChanged()
{
	static FTARibbonEditorPreviewRequestChanged Event;
	return Event;
}

void UTARibbonComponent::SetEditorPreviewSuspended(bool bSuspended)
{
	if (bEditorPreviewSuspended == bSuspended) { return; }
	bEditorPreviewSuspended = bSuspended;
	if (bSuspended)
	{
		StopEditorPreviewState();
	}
	else if (bEditorPreviewRequested && IsRegistered())
	{
		if (bResetRequiredAfterEdit) { SetRuntimeError(TEXT("Preview settings changed. Run Reset Simulation.")); }
		else { ResetSimulation(); }
	}
}

bool UTARibbonComponent::GetEditorPreviewSourceTransform(FTransform& OutTransform) const
{
	if (const UStaticMeshComponent* Source = ResolveSourceMeshComponent())
	{
		OutTransform = Source->GetComponentTransform();
		return true;
	}
	return false;
}

void UTARibbonComponent::RestoreEditorPreviewRequest(bool bRequireReset)
{
	bEditorPreviewRequested = true;
	bResetRequiredAfterEdit = bRequireReset;
	if (bRequireReset)
	{
		StopEditorPreviewState();
		SetRuntimeError(TEXT("Preview source or physical settings changed during reconstruction. Run Reset Simulation."));
	}
	else if (!bEditorPreviewSuspended && IsRegistered()) { ResetSimulation(); }
}
#endif

void UTARibbonComponent::StartEditorPreview()
{
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->WorldType != EWorldType::Editor || IsTemplate() || !IsRegistered()) { return; }
	bEditorPreviewRequested = true;
	OnEditorPreviewRequestChanged().Broadcast(this, true);
	if (!bEditorPreviewSuspended && !bEditorPreviewActive) { ResetSimulation(); }
#endif
}

void UTARibbonComponent::StopEditorPreviewState()
{
	if (!GetWorld() || GetWorld()->WorldType != EWorldType::Editor) { return; }
	bEditorPreviewActive = false;
	UnregisterFromWorldSubsystem();
	DestroyRuntimeRenderer();
	ReleaseGpuSimulation();
	PreparedSimulation.Reset();
	AccumulatedSeconds = SimulatedSeconds = 0.0;
	PendingManualTicks = 0;
	bGpuCreatePending = false;
	bRuntimeStarted = false;
	SimulationStatus = ETARibbonSimulationStatus::Uninitialized;
	StatusMessage = TEXT("编辑器预览已停止，源网格显示已恢复。");
}

void UTARibbonComponent::StopEditorPreview()
{
#if WITH_EDITOR
	if (!GetWorld() || GetWorld()->WorldType != EWorldType::Editor) { return; }
	bEditorPreviewRequested = false;
	bEditorPreviewSuspended = false;
	StopEditorPreviewState();
	OnEditorPreviewRequestChanged().Broadcast(this, false);
#endif
}

void UTARibbonComponent::ResetSimulation()
{
	bRuntimeStarted = bAutoStart || bRuntimeStarted;
	if (!CanSimulateInCurrentWorld())
	{
		ValidateRibbonSetup();
		return;
	}
	if (GetWorld()->WorldType == EWorldType::Editor) { bRuntimeStarted = true; }
	FString Error;
	if (!PrepareSimulation(Error))
	{
		SetRuntimeError(Error);
		UE_LOG(LogTARibbon, Error, TEXT("%s: reset failed: %s"), *GetPathName(), *Error);
		return;
	}
	if (!CreateOrRefreshRuntimeRenderer(Error))
	{
		SetRuntimeError(Error);
		UE_LOG(LogTARibbon, Error, TEXT("%s: renderer reset failed: %s"), *GetPathName(), *Error);
		return;
	}
	if (GetWorld()->WorldType == EWorldType::Editor) { bEditorPreviewActive = true; }
	bResetRequiredAfterEdit = false;
	RegisterWithWorldSubsystem();
	RefreshRuntimeRenderState();
#if WITH_EDITOR
	if (bEditorPreviewActive) { OnEditorPreviewRequestChanged().Broadcast(this, true); }
#endif
}

void UTARibbonComponent::StepOneFixedTick()
{
	if (bResetRequiredAfterEdit || SimulationStatus == ETARibbonSimulationStatus::Error)
	{
		return; // A manual step must not bypass an explicit Reset requirement.
	}
	if (!PreparedSimulation && CanSimulateInCurrentWorld())
	{
		ResetSimulation();
	}
	if (PreparedSimulation && SimulationStatus != ETARibbonSimulationStatus::Error)
	{
		++PendingManualTicks;
		SimulationStatus = ETARibbonSimulationStatus::Ready;
		StatusMessage = TEXT("One fixed tick queued.");
	}
}

void UTARibbonComponent::SetSimulationPaused(bool bInPaused)
{
	bPaused = bInPaused;
	if (!bInPaused)
	{
		bRuntimeStarted = true;
	}
	if (bResetRequiredAfterEdit || SimulationStatus == ETARibbonSimulationStatus::Error) { return; }
	SimulationStatus = bPaused ? ETARibbonSimulationStatus::Paused : ETARibbonSimulationStatus::Ready;
	StatusMessage = bPaused ? TEXT("Simulation paused.") : TEXT("Simulation resumed.");
}

FPrimitiveSceneProxy* UTARibbonComponent::CreateSceneProxy()
{
	// Keep UStaticMeshComponent as the serialized ABI only. The authored cloth mesh is
	// resolved from the parent/owner, and only the private transient renderer may
	// submit either the source mesh or GPU-deformed vertices.
	return nullptr;
}

FBoxSphereBounds UTARibbonComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0f).TransformBy(LocalToWorld);
}

bool UTARibbonComponent::CreateOrRefreshRuntimeRenderer(FString& OutError)
{
	UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent(&OutError);
	if (!SourceComponent)
	{
		return false;
	}

	if (RuntimeRenderer && RuntimeSourceComponent.Get() != SourceComponent)
	{
		DestroyRuntimeRenderer();
	}

	if (!RuntimeRenderer)
	{
		AActor* Owner = GetOwner();
		if (!Owner)
		{
			OutError = TEXT("TARibbon cannot create its runtime renderer without an owning Actor.");
			return false;
		}

		RuntimeRenderer = NewObject<UTARibbonRenderComponent>(
			this, TEXT("TARibbonRuntimeRenderer"), RF_Transient | RF_DuplicateTransient);
		if (!RuntimeRenderer)
		{
			OutError = TEXT("TARibbon failed to create its private runtime render component.");
			return false;
		}

		RuntimeRenderer->SetupAttachment(SourceComponent);
		RuntimeRenderer->SetRelativeTransform(FTransform::Identity);
		RuntimeRenderer->InitializeFromSource(this, SourceComponent);
		RuntimeRenderer->RegisterComponentWithWorld(GetWorld());
		if (!RuntimeRenderer->IsRegistered())
		{
			DestroyRuntimeRenderer();
			OutError = TEXT("TARibbon failed to register its private runtime render component.");
			return false;
		}
	}
	else
	{
		RuntimeRenderer->InitializeFromSource(this, SourceComponent);
		if (!RuntimeRenderer->IsRegistered())
		{
			RuntimeRenderer->RegisterComponentWithWorld(GetWorld());
		}
	}
	if (!RuntimeRenderer || !RuntimeRenderer->IsRegistered())
	{
		DestroyRuntimeRenderer();
		OutError = TEXT("TARibbon failed to register its private runtime render component.");
		return false;
	}

	SuppressSourceRendering(SourceComponent);
	OutError.Reset();
	return true;
}

void UTARibbonComponent::DestroyRuntimeRenderer()
{
	RestoreSourceRendering();
	if (!RuntimeRenderer)
	{
		return;
	}

	UTARibbonRenderComponent* RendererToDestroy = RuntimeRenderer;
	RuntimeRenderer = nullptr;
	if (IsValid(RendererToDestroy) && !RendererToDestroy->IsBeingDestroyed())
	{
		RendererToDestroy->DestroyComponent();
	}
}

void UTARibbonComponent::SuppressSourceRendering(UStaticMeshComponent* SourceComponent)
{
	if (!SourceComponent)
	{
		return;
	}
	if (bSourceRenderingSuppressed && RuntimeSourceComponent.Get() == SourceComponent)
	{
		return;
	}

	RestoreSourceRendering();
	RuntimeSourceComponent = SourceComponent;
	bSourceWasVisible = SourceComponent->GetVisibleFlag();
	bSourceWasHiddenInGame = SourceComponent->bHiddenInGame;
	bSourceUsedEditorHiding = GetWorld() && GetWorld()->WorldType == EWorldType::Editor;
#if WITH_EDITOR
	if (bSourceUsedEditorHiding)
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		bSourceWasTemporarilyHiddenInEditor = SourceComponent->IsTemporarilyHiddenInEditor(false);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		SourceComponent->SetIsTemporarilyHiddenInEditor(true);
	}
	else
#endif
	{
		SourceComponent->SetVisibility(false, false);
		SourceComponent->SetHiddenInGame(true, false);
	}
	bSourceRenderingSuppressed = true;
}

void UTARibbonComponent::RestoreSourceRendering()
{
	if (bSourceRenderingSuppressed)
	{
		if (UStaticMeshComponent* SourceComponent = RuntimeSourceComponent.Get())
		{
#if WITH_EDITOR
			if (bSourceUsedEditorHiding)
			{
				SourceComponent->SetIsTemporarilyHiddenInEditor(bSourceWasTemporarilyHiddenInEditor);
			}
			else
#endif
			{
				SourceComponent->SetVisibility(bSourceWasVisible, false);
				SourceComponent->SetHiddenInGame(bSourceWasHiddenInGame, false);
			}
		}
	}
	RuntimeSourceComponent.Reset();
	bSourceRenderingSuppressed = false;
	bSourceUsedEditorHiding = false;
}

void UTARibbonComponent::RefreshRuntimeRenderState()
{
	if (RuntimeRenderer)
	{
		RuntimeRenderer->MarkRenderStateDirty();
		RuntimeRenderer->UpdateBounds();
	}
}

void UTARibbonComponent::OnRegister()
{
	Super::OnRegister();
	bRuntimeStarted = bAutoStart;
	if (bResetRequiredAfterEdit)
	{
		SetRuntimeError(TEXT("TARibbon settings changed. Run Reset Simulation to rebuild GPU state."));
		return;
	}
	if (CanSimulateInCurrentWorld())
	{
		FString Error;
		if (PrepareSimulation(Error) && CreateOrRefreshRuntimeRenderer(Error))
		{
			if (GetWorld()->WorldType == EWorldType::Editor)
			{
				bEditorPreviewActive = true;
				bRuntimeStarted = true;
			}
			RegisterWithWorldSubsystem();
			RefreshRuntimeRenderState();
		}
		else
		{
			SetRuntimeError(Error);
			UE_LOG(LogTARibbon, Error, TEXT("%s: initialization failed: %s"), *GetPathName(), *Error);
		}
	}
}

void UTARibbonComponent::OnUnregister()
{
	bEditorPreviewActive = false;
	UnregisterFromWorldSubsystem();
	DestroyRuntimeRenderer();
	Super::OnUnregister();
	ReleaseGpuSimulation();
	PreparedSimulation.Reset();
}

void UTARibbonComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	bEditorPreviewActive = false;
	UnregisterFromWorldSubsystem();
	DestroyRuntimeRenderer();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
	ReleaseGpuSimulation();
	PreparedSimulation.Reset();
}

#if WITH_EDITOR
void UTARibbonComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const FName MemberPropertyName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: PropertyName;
	const bool bPresetSelectionChange =
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, ClothPreset);
	const bool bPresetManagedPropertyChange =
		UE::TARibbon::Private::IsClothPresetManagedProperty(MemberPropertyName);
	if (bPresetSelectionChange && ClothPreset != ETARibbonClothPreset::Custom)
	{
		ApplyClothPresetValues(ClothPreset);
	}
	else if (bPresetManagedPropertyChange)
	{
		ClothPreset = ETARibbonClothPreset::Custom;
	}
	const bool bHotWindRoutingChange =
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, WindSourceMode) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(UTARibbonComponent, SpecificWindActor);
	const bool bResetRequiredChange =
		bPresetManagedPropertyChange ||
		(bPresetSelectionChange && ClothPreset != ETARibbonClothPreset::Custom);
	if (CanSimulateInCurrentWorld() && bResetRequiredChange)
	{
		bResetRequiredAfterEdit = true;
		SetRuntimeError(TEXT("A TARibbon property changed during simulation. Run Reset Simulation to rebuild GPU state."));
	}
	else if (bHotWindRoutingChange)
	{
		FSceneWindFieldEvaluationParameters WindParameters;
		FString WindWarning;
		BuildSceneWindSnapshot(WindParameters, WindWarning);
	}
	else
	{
		FString Error;
		if (!CheckSetup(Error, true))
		{
			StatusMessage = MoveTemp(Error);
			SimulationStatus = ETARibbonSimulationStatus::Uninitialized;
		}
	}
	RefreshRuntimeRenderState();
	if (bEditorPreviewRequested) { OnEditorPreviewRequestChanged().Broadcast(this, true); }
	// Component reconstruction can replace this object; never access it after Super.
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

bool UTARibbonComponent::BuildWorldDispatch(float DeltaTime, FTARibbonWorldDispatchRequest& OutRequest)
{
	if (!CanSimulateInCurrentWorld()) { return false; }
	RefreshDiagnostics();
	if (!PreparedSimulation || !GpuSimulation || bResetRequiredAfterEdit || SimulationStatus == ETARibbonSimulationStatus::Error)
	{
		return false;
	}
	UStaticMeshComponent* SourceComponent = ResolveSourceMeshComponent();
	if (!SourceComponent)
	{
		SetRuntimeError(TEXT("The source cloth StaticMeshComponent is no longer available."));
		return false;
	}
	if (SourceComponent->GetStaticMesh() != SimulationSourceMesh.Get())
	{
		SetRuntimeError(TEXT("The source Static Mesh changed after initialization. Run Reset Simulation to rebuild topology and GPU state."));
		return false;
	}
	if (SourceComponent->GetMaterial(0) != SimulationSourceMaterial.Get())
	{
		SetRuntimeError(TEXT("The source material changed after initialization. Run Reset Simulation to refresh the TARibbon renderer."));
		return false;
	}
	const UTARibbonMeshData* CurrentMeshData = GetRibbonMeshData();
	if (!CurrentMeshData || !CurrentMeshData->IsInternallyConsistent() ||
		CurrentMeshData->TopologySignature != SimulationBakeSignature)
	{
		SetRuntimeError(TEXT("The source Bake v2 data changed or became stale after initialization. Re-bake if needed, then run Reset Simulation."));
		return false;
	}
	if (!SourceComponent->GetComponentTransform().Equals(SimulationTransform, 1.0e-4))
	{
		SetRuntimeError(TEXT("The source cloth mesh transform changed after initialization. TARibbon history is frozen; run Reset Simulation."));
		return false;
	}
	if (ComputeResetRequiredConfigurationHash() != SimulationConfigurationHash)
	{
		SetRuntimeError(TEXT("A TARibbon physical, solver, safety, or ArtWind parameter changed after initialization. Run Reset Simulation."));
		return false;
	}

	uint32 TickCount = 0;
	if (bRuntimeStarted && !bPaused)
	{
		AccumulatedSeconds += FMath::Max(0.0f, DeltaTime);
		const uint64 DueTicks = static_cast<uint64>(FMath::FloorToInt64(AccumulatedSeconds / FixedTimeStep));
		const uint64 ExecutedTicks = FMath::Min<uint64>(DueTicks, static_cast<uint64>(MaxTicksPerFrame));
		const uint64 DroppedThisFrame = DueTicks - ExecutedTicks;
		DroppedTicks += static_cast<int64>(DroppedThisFrame);
		AccumulatedSeconds -= static_cast<double>(DueTicks) * FixedTimeStep;
		TickCount = static_cast<uint32>(ExecutedTicks);
	}
	if (PendingManualTicks > 0)
	{
		const uint32 ManualTicks = FMath::Min(static_cast<uint32>(PendingManualTicks),
			static_cast<uint32>(MaxTicksPerFrame) - TickCount);
		TickCount += ManualTicks;
		PendingManualTicks -= static_cast<int32>(ManualTicks);
	}

	OutRequest.Simulation = GpuSimulation;
	OutRequest.Prepared = PreparedSimulation;
	OutRequest.FixedTickCount = TickCount;
	OutRequest.StartTime = SimulatedSeconds;
	FString WindWarning;
	OutRequest.bUseSceneWind = BuildSceneWindSnapshot(OutRequest.SceneWindParameters, WindWarning);
	const UWorld* World = GetWorld();
	const double WorldTimeSeconds = World ? static_cast<double>(World->GetTimeSeconds()) : 0.0;
	OutRequest.SceneWindTimeOffset = UE::TARibbon::Private::ComputeSceneWindTimeOffset(
		WorldTimeSeconds,
		OutRequest.StartTime,
		TickCount,
		FixedTimeStep);
	OutRequest.bCreate = bGpuCreatePending;
	OutRequest.bReadbackDiagnostics = bReadbackGpuDiagnostics;
	bGpuCreatePending = false;
	SimulatedSeconds += static_cast<double>(TickCount) * FixedTimeStep;
	SimulationStatus = bPaused || !bRuntimeStarted ? ETARibbonSimulationStatus::Paused : ETARibbonSimulationStatus::Running;
	StatusMessage = TickCount > 0 ? TEXT("GPU XPBD simulation running.") : TEXT("GPU state rendered; no fixed tick was due this frame.");
	if (!WindWarning.IsEmpty() && WindSourceMode != ETARibbonWindSourceMode::Disabled)
	{
		StatusMessage += TEXT(" SceneWind warning: ") + WindWarning;
	}
	if (!CollisionWarning.IsEmpty()) { StatusMessage += TEXT(" Collision warning: ") + CollisionWarning; }
	return true;
}

void UTARibbonComponent::RegisterWithWorldSubsystem()
{
	if (!bRegisteredWithSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			if (UTARibbonWorldSubsystem* Subsystem = World->GetSubsystem<UTARibbonWorldSubsystem>())
			{
				Subsystem->RegisterRibbon(this);
				bRegisteredWithSubsystem = true;
			}
		}
	}
}

void UTARibbonComponent::UnregisterFromWorldSubsystem()
{
	if (bRegisteredWithSubsystem)
	{
		if (UWorld* World = GetWorld())
		{
			if (UTARibbonWorldSubsystem* Subsystem = World->GetSubsystem<UTARibbonWorldSubsystem>())
			{
				Subsystem->UnregisterRibbon(this);
			}
		}
		bRegisteredWithSubsystem = false;
	}
}

void UTARibbonComponent::ReleaseGpuSimulation()
{
	if (!GpuSimulation)
	{
		return;
	}
	check(IsInGameThread());
	TSharedPtr<FTARibbonGpuSimulation, ESPMode::ThreadSafe> Releasing = MoveTemp(GpuSimulation);
	ENQUEUE_RENDER_COMMAND(TARibbonReleaseSimulation)(
		[Releasing = MoveTemp(Releasing)](FRHICommandListImmediate& RHICmdList)
		{
			Releasing->Release();
		});
}

void UTARibbonComponent::SetRuntimeError(const FString& Error)
{
	if (CanSimulateInCurrentWorld()) { bResetRequiredAfterEdit = true; }
	SimulationStatus = ETARibbonSimulationStatus::Error;
	StatusMessage = Error.IsEmpty() ? TEXT("Unknown TARibbon error.") : Error;
}

void UTARibbonComponent::RefreshDiagnostics()
{
	if (!GpuSimulation)
	{
		return;
	}
	const TSharedPtr<FTARibbonRenderState, ESPMode::ThreadSafe> RenderState = GpuSimulation->GetRenderState();
	if (RenderState->HasFailed())
	{
		SetRuntimeError(FString::Printf(TEXT("TARibbon GPU initialization failed: %s (code %u)."),
			RenderState->GetFailureMessage(), RenderState->GetFailureCode()));
		return;
	}
	FUintVector4 Diagnostics;
	if (RenderState->TryGetLatestDiagnostics(Diagnostics))
	{
		GpuBadTriangles = static_cast<int32>(Diagnostics.X);
		GpuBadHinges = static_cast<int32>(Diagnostics.Y);
		GpuWindClamps = static_cast<int32>(Diagnostics.Z);
		GpuBadContacts = static_cast<int32>(Diagnostics.W);
	}
}
