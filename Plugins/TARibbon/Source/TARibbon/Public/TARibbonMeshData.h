#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "TARibbonMeshData.generated.h"

class UStaticMesh;

UENUM(BlueprintType)
enum class ETARibbonMeshBakeStatus : uint8
{
	Invalid,
	Valid
};

/** One render corner in the imported LOD0 MeshDescription. */
USTRUCT(BlueprintType)
struct TARIBBON_API FTARibbonRenderVertexBinding
{
	GENERATED_BODY()

	/** Dense simulation vertex index corresponding to this vertex instance. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 SimulationVertexIndex = INDEX_NONE;

	/** Imported LOD0 vertex-instance ID, retained for editor diagnostics only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 SourceVertexInstanceID = INDEX_NONE;

	/** Dense simulation triangle used by the deformation binding. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 PhysicalTriangleIndex = INDEX_NONE;

	/** One-hot for source corners today; stored generally for future render meshes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	FVector3f Barycentric = FVector3f(1.0f, 0.0f, 0.0f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	float NormalOffsetMeters = 0.0f;

	/** Imported tangent-basis handedness. Always -1 or +1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	float TangentSign = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	FVector2f UV0 = FVector2f::ZeroVector;
};

/** Cookable, deterministic LOD0 simulation data baked from a static mesh. */
UCLASS(BlueprintType)
class TARIBBON_API UTARibbonMeshData final : public UAssetUserData
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentBakeVersion = 2;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 BakeVersion = CurrentBakeVersion;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	ETARibbonMeshBakeStatus Status = ETARibbonMeshBakeStatus::Invalid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 SimulationVertexCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 TriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	int32 RenderVertexCount = 0;

	/** FVertexID order used to create the dense arrays below. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<int32> SourceVertexIDs;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<FVector3f> RestPositions;

	/** One byte per simulation vertex. Values are always zero or one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<uint8> PinnedVertices;

	/** One float per simulation vertex. Values are finite and in [0,1]. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon|Wind")
	TArray<float> WindResponse;

	/** One float per simulation vertex. Values are finite and in [0,1]. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon|Wind")
	TArray<float> Permeability;

	/** Dense simulation vertex indices, three per source triangle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<int32> TriangleIndices;

	/** One entry per imported LOD0 FVertexInstanceID, sorted by that ID. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<FTARibbonRenderVertexBinding> RenderBindings;

	/** Render-binding indices, three per source triangle, in TriangleIndices order. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "TA Ribbon")
	TArray<int32> RenderIndices;

	/** Stable FNV-1a signature of the LOD0 geometry, render corners and RGB vertex masks. */
	UPROPERTY(VisibleAnywhere, Category = "TA Ribbon")
	uint64 TopologySignature = 0;

	bool IsInternallyConsistent() const;
	bool IsPinned(int32 SimulationVertexIndex) const
	{
		return PinnedVertices.IsValidIndex(SimulationVertexIndex) && PinnedVertices[SimulationVertexIndex] != 0;
	}

#if WITH_EDITOR
	/** Detects topology-changing reimports without mutating this data or the mesh. */
	bool IsTopologyCurrent(const UStaticMesh* StaticMesh, FText& OutError) const;
#endif
};

#if WITH_EDITOR
/** Strict v2 authoring bake. Returns false without changing the mesh on validation failure. */
class TARIBBON_API FTARibbonMeshBaker
{
public:
	static bool BakeLOD0(UStaticMesh* StaticMesh, FText& OutError);
};
#endif
