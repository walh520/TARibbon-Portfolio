#include "TARibbonMeshData.h"

#include "Engine/StaticMesh.h"

bool UTARibbonMeshData::IsInternallyConsistent() const
{
	const int32 NumVertices = RestPositions.Num();
	if (BakeVersion != CurrentBakeVersion || Status != ETARibbonMeshBakeStatus::Valid || NumVertices < 3 ||
		SourceVertexIDs.Num() != NumVertices || PinnedVertices.Num() != NumVertices ||
		WindResponse.Num() != NumVertices || Permeability.Num() != NumVertices ||
		TriangleIndices.IsEmpty() || TriangleIndices.Num() % 3 != 0 ||
		RenderBindings.IsEmpty() || RenderIndices.Num() != TriangleIndices.Num() || TopologySignature == 0 ||
		SimulationVertexCount != NumVertices || TriangleCount != TriangleIndices.Num() / 3 ||
		RenderVertexCount != RenderBindings.Num())
	{
		return false;
	}

	int32 NumPinned = 0;
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		const FVector3f& Position = RestPositions[Vertex];
		if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z) ||
			!FMath::IsFinite(WindResponse[Vertex]) || WindResponse[Vertex] < 0.0f || WindResponse[Vertex] > 1.0f ||
			!FMath::IsFinite(Permeability[Vertex]) || Permeability[Vertex] < 0.0f || Permeability[Vertex] > 1.0f ||
			PinnedVertices[Vertex] > 1u)
		{
			return false;
		}
		NumPinned += PinnedVertices[Vertex] != 0 ? 1 : 0;
	}
	if (NumPinned == 0 || NumPinned == NumVertices)
	{
		return false;
	}
	for (const int32 Index : TriangleIndices)
	{
		if (Index < 0 || Index >= NumVertices)
		{
			return false;
		}
	}
	for (const FTARibbonRenderVertexBinding& Binding : RenderBindings)
	{
		if (Binding.SimulationVertexIndex < 0 || Binding.SimulationVertexIndex >= NumVertices ||
			Binding.PhysicalTriangleIndex < 0 || Binding.PhysicalTriangleIndex >= TriangleCount)
		{
			return false;
		}
	}
	for (const int32 Index : RenderIndices)
	{
		if (!RenderBindings.IsValidIndex(Index))
		{
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR

#include "Algo/Sort.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace UE::TARibbon::Private
{
constexpr float ColorEndpointTolerance = 0.05f;
constexpr double DegenerateAreaSquaredTolerance = 1.0e-12;

struct FEdgeUse
{
	int32 Triangle = INDEX_NONE;
	int8 Direction = 0;
};

static uint64 HashBytes(uint64 Hash, const void* Data, SIZE_T Size)
{
	const uint8* Bytes = static_cast<const uint8*>(Data);
	for (SIZE_T Index = 0; Index < Size; ++Index)
	{
		Hash ^= Bytes[Index];
		Hash *= 1099511628211ull;
	}
	return Hash;
}

template <typename T>
static uint64 HashValue(uint64 Hash, const T& Value)
{
	return HashBytes(Hash, &Value, sizeof(T));
}

static uint64 MakeEdgeKey(int32 A, int32 B)
{
	const uint32 MinVertex = static_cast<uint32>(FMath::Min(A, B));
	const uint32 MaxVertex = static_cast<uint32>(FMath::Max(A, B));
	return (static_cast<uint64>(MinVertex) << 32) | MaxVertex;
}

static bool Fail(FText& OutError, const FString& Message)
{
	OutError = FText::FromString(Message);
	return false;
}

static bool ComputeTopologySignature(const FMeshDescription& Mesh, uint64& OutSignature, FText& OutError)
{
	FStaticMeshConstAttributes Attributes(Mesh);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	const TVertexInstanceAttributesConstRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	if (!Positions.IsValid() || !Colors.IsValid() || !UVs.IsValid() || UVs.GetNumChannels() < 1 ||
		!BinormalSigns.IsValid())
	{
		return Fail(OutError, TEXT("LOD0 is missing positions, vertex colors, UV0, or tangent handedness."));
	}

	TArray<FVertexID> VertexIDs;
	for (const FVertexID VertexID : Mesh.Vertices().GetElementIDs())
	{
		VertexIDs.Add(VertexID);
	}
	VertexIDs.Sort([](FVertexID A, FVertexID B) { return A.GetValue() < B.GetValue(); });
	TMap<FVertexID, int32> DenseVertex;
	for (int32 Index = 0; Index < VertexIDs.Num(); ++Index)
	{
		DenseVertex.Add(VertexIDs[Index], Index);
	}
	TArray<FTriangleID> TriangleIDs;
	for (const FTriangleID TriangleID : Mesh.Triangles().GetElementIDs())
	{
		TriangleIDs.Add(TriangleID);
	}
	TriangleIDs.Sort([](FTriangleID A, FTriangleID B) { return A.GetValue() < B.GetValue(); });
	TArray<FVertexInstanceID> InstanceIDs;
	for (const FVertexInstanceID InstanceID : Mesh.VertexInstances().GetElementIDs())
	{
		InstanceIDs.Add(InstanceID);
	}
	InstanceIDs.Sort([](FVertexInstanceID A, FVertexInstanceID B) { return A.GetValue() < B.GetValue(); });

	uint64 Signature = 14695981039346656037ull;
	const int32 NumVertices = VertexIDs.Num();
	const int32 NumTriangles = TriangleIDs.Num();
	const int32 NumInstances = InstanceIDs.Num();
	Signature = HashValue(Signature, NumVertices);
	Signature = HashValue(Signature, NumTriangles);
	Signature = HashValue(Signature, NumInstances);
	for (const FVertexID VertexID : VertexIDs)
	{
		const int32 SourceID = VertexID.GetValue();
		Signature = HashValue(Signature, SourceID);
		Signature = HashValue(Signature, Positions[VertexID]);
	}
	for (const FVertexInstanceID InstanceID : InstanceIDs)
	{
		const int32 SourceInstanceID = InstanceID.GetValue();
		const FVertexID VertexID = Mesh.GetVertexInstanceVertex(InstanceID);
		const int32* DenseIndex = DenseVertex.Find(VertexID);
		if (!DenseIndex)
		{
			return Fail(OutError, TEXT("LOD0 contains an invalid vertex-instance vertex reference."));
		}
		Signature = HashValue(Signature, SourceInstanceID);
		Signature = HashValue(Signature, *DenseIndex);
		Signature = HashValue(Signature, Colors[InstanceID]);
		Signature = HashValue(Signature, UVs.Get(InstanceID, 0));
		Signature = HashValue(Signature, BinormalSigns[InstanceID]);
	}
	for (const FTriangleID TriangleID : TriangleIDs)
	{
		for (const FVertexInstanceID InstanceID : Mesh.GetTriangleVertexInstances(TriangleID))
		{
			const int32 SourceInstanceID = InstanceID.GetValue();
			Signature = HashValue(Signature, SourceInstanceID);
		}
	}
	OutSignature = Signature == 0 ? 1 : Signature;
	return true;
}
}

bool UTARibbonMeshData::IsTopologyCurrent(const UStaticMesh* StaticMesh, FText& OutError) const
{
	using namespace UE::TARibbon::Private;
	OutError = FText::GetEmpty();
	if (!IsValid(StaticMesh) || StaticMesh->GetNumSourceModels() != 1)
	{
		return Fail(OutError, TEXT("Topology freshness requires a valid one-LOD StaticMesh."));
	}
	const FMeshDescription* Mesh = StaticMesh->GetMeshDescription(0);
	uint64 CurrentSignature = 0;
	if (!Mesh || !ComputeTopologySignature(*Mesh, CurrentSignature, OutError))
	{
		return false;
	}
	if (CurrentSignature != TopologySignature)
	{
		return Fail(OutError, TEXT("TA Ribbon bake is stale: current LOD0 topology does not match its stored signature."));
	}
	return true;
}

bool FTARibbonMeshBaker::BakeLOD0(UStaticMesh* StaticMesh, FText& OutError)
{
	using namespace UE::TARibbon::Private;
	OutError = FText::GetEmpty();
	if (!IsValid(StaticMesh))
	{
		return Fail(OutError, TEXT("TA Ribbon bake requires a valid StaticMesh."));
	}
	if (StaticMesh->GetNaniteSettings().bEnabled)
	{
		return Fail(OutError, TEXT("TA Ribbon v2 requires Nanite to be disabled."));
	}
	if (StaticMesh->GetNumSourceModels() != 1)
	{
		return Fail(OutError, TEXT("TA Ribbon v2 requires exactly one source LOD (LOD0)."));
	}

	const FMeshDescription* Mesh = StaticMesh->GetMeshDescription(0);
	if (!Mesh || Mesh->Vertices().Num() == 0 || Mesh->Triangles().Num() == 0)
	{
		return Fail(OutError, TEXT("LOD0 has no valid MeshDescription geometry."));
	}
	if (Mesh->PolygonGroups().Num() != 1)
	{
		return Fail(OutError, TEXT("TA Ribbon v2 requires exactly one polygon group/section."));
	}

	FStaticMeshConstAttributes Attributes(*Mesh);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	const TVertexInstanceAttributesConstRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	if (!Positions.IsValid() || !Colors.IsValid())
	{
		return Fail(OutError, TEXT("LOD0 must contain imported vertex positions and vertex colors."));
	}
	if (!UVs.IsValid() || UVs.GetNumChannels() < 1 || !BinormalSigns.IsValid())
	{
		return Fail(OutError, TEXT("LOD0 must contain UV0 and a valid tangent basis."));
	}

	TArray<FVertexID> VertexIDs;
	for (const FVertexID VertexID : Mesh->Vertices().GetElementIDs())
	{
		VertexIDs.Add(VertexID);
	}
	VertexIDs.Sort([](FVertexID A, FVertexID B) { return A.GetValue() < B.GetValue(); });
	TMap<FVertexID, int32> DenseVertex;
	DenseVertex.Reserve(VertexIDs.Num());

	UTARibbonMeshData* Baked = NewObject<UTARibbonMeshData>(GetTransientPackage());
	Baked->SourceVertexIDs.Reserve(VertexIDs.Num());
	Baked->RestPositions.Reserve(VertexIDs.Num());
	Baked->PinnedVertices.Reserve(VertexIDs.Num());
	Baked->WindResponse.Reserve(VertexIDs.Num());
	Baked->Permeability.Reserve(VertexIDs.Num());

	int32 NumPinned = 0;
	bool bHasNonDefaultColor = false;
	for (int32 DenseIndex = 0; DenseIndex < VertexIDs.Num(); ++DenseIndex)
	{
		const FVertexID VertexID = VertexIDs[DenseIndex];
		DenseVertex.Add(VertexID, DenseIndex);
		Baked->SourceVertexIDs.Add(VertexID.GetValue());
		const FVector3f Position = Positions[VertexID];
		if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
		{
			return Fail(OutError, FString::Printf(TEXT("Source vertex %d has non-finite position."), VertexID.GetValue()));
		}
		Baked->RestPositions.Add(Position);

		const TArray<FVertexInstanceID> Instances(Mesh->GetVertexVertexInstanceIDs(VertexID));
		if (Instances.IsEmpty())
		{
			return Fail(OutError, FString::Printf(TEXT("Vertex %d has no vertex instances."), VertexID.GetValue()));
		}

		TOptional<FVector3f> VertexRGB;
		for (const FVertexInstanceID InstanceID : Instances)
		{
			const FVector4f Color = Colors[InstanceID];
			const FVector3f RGB(Color.X, Color.Y, Color.Z);
			bHasNonDefaultColor |= !RGB.Equals(FVector3f(1.0f), UE_SMALL_NUMBER);
			if (!FMath::IsFinite(RGB.X) || !FMath::IsFinite(RGB.Y) || !FMath::IsFinite(RGB.Z))
			{
				return Fail(OutError, FString::Printf(TEXT("Vertex instance %d has non-finite RGB vertex color."), InstanceID.GetValue()));
			}
			const float Red = RGB.X;
			if (!FMath::IsFinite(Red) || Red < 0.0f || Red > 1.0f ||
				(Red > ColorEndpointTolerance && Red < 1.0f - ColorEndpointTolerance))
			{
				return Fail(OutError, FString::Printf(TEXT("Vertex instance %d has non-binary red %.6g; expected <= %.2f or >= %.2f."), InstanceID.GetValue(), Red, ColorEndpointTolerance, 1.0f - ColorEndpointTolerance));
			}
			if (RGB.Y < 0.0f || RGB.Y > 1.0f || RGB.Z < 0.0f || RGB.Z > 1.0f)
			{
				return Fail(OutError, FString::Printf(TEXT("Vertex instance %d has wind response/permeability outside [0,1] (G=%.6g, B=%.6g)."), InstanceID.GetValue(), RGB.Y, RGB.Z));
			}
			if (VertexRGB.IsSet() && !VertexRGB.GetValue().Equals(RGB, UE_SMALL_NUMBER))
			{
				return Fail(OutError, FString::Printf(TEXT("Duplicated vertex instances for source vertex %d disagree on RGB vertex color."), VertexID.GetValue()));
			}
			VertexRGB = RGB;
		}
		const FVector3f RGB = VertexRGB.GetValue();
		Baked->PinnedVertices.Add(RGB.X >= 0.5f ? 1u : 0u);
		Baked->WindResponse.Add(RGB.Y);
		Baked->Permeability.Add(RGB.Z);
		NumPinned += RGB.X >= 0.5f ? 1 : 0;
	}

	if (!bHasNonDefaultColor)
	{
		return Fail(OutError, TEXT("LOD0 has no authored/imported vertex color data (all vertex-instance colors are the default white). Reimport with Vertex Color Import Option = Replace."));
	}
	if (NumPinned == 0 || NumPinned == VertexIDs.Num())
	{
		return Fail(OutError, TEXT("Pin mask must contain at least one pinned and one free simulation vertex."));
	}

	// Distinct FVertexIDs at the same position are ambiguous and forbidden in v2.
	for (int32 A = 0; A < Baked->RestPositions.Num(); ++A)
	{
		for (int32 B = A + 1; B < Baked->RestPositions.Num(); ++B)
		{
			if (Baked->RestPositions[A].Equals(Baked->RestPositions[B], UE_SMALL_NUMBER))
			{
				return Fail(OutError, FString::Printf(TEXT("Source vertices %d and %d duplicate the same position."), Baked->SourceVertexIDs[A], Baked->SourceVertexIDs[B]));
			}
		}
	}

	TArray<FTriangleID> TriangleIDs;
	for (const FTriangleID TriangleID : Mesh->Triangles().GetElementIDs())
	{
		TriangleIDs.Add(TriangleID);
	}
	TriangleIDs.Sort([](FTriangleID A, FTriangleID B) { return A.GetValue() < B.GetValue(); });
	TMap<FTriangleID, int32> DenseTriangleByID;
	for (int32 Index = 0; Index < TriangleIDs.Num(); ++Index)
	{
		DenseTriangleByID.Add(TriangleIDs[Index], Index);
	}
	TMap<uint64, TArray<FEdgeUse>> EdgeUses;
	TSet<FIntVector> UniqueTriangles;
	TArray<TArray<int32>> TriangleAdjacency;
	TriangleAdjacency.SetNum(TriangleIDs.Num());

	for (int32 DenseTriangle = 0; DenseTriangle < TriangleIDs.Num(); ++DenseTriangle)
	{
		const TArrayView<const FVertexID> TriangleVertices = Mesh->GetTriangleVertices(TriangleIDs[DenseTriangle]);
		int32 Indices[3] = { DenseVertex.FindChecked(TriangleVertices[0]), DenseVertex.FindChecked(TriangleVertices[1]), DenseVertex.FindChecked(TriangleVertices[2]) };
		if (Indices[0] == Indices[1] || Indices[1] == Indices[2] || Indices[2] == Indices[0])
		{
			return Fail(OutError, FString::Printf(TEXT("Triangle %d repeats a vertex."), TriangleIDs[DenseTriangle].GetValue()));
		}
		const FVector3f Cross = (Baked->RestPositions[Indices[1]] - Baked->RestPositions[Indices[0]]) ^ (Baked->RestPositions[Indices[2]] - Baked->RestPositions[Indices[0]]);
		if (static_cast<double>(Cross.SquaredLength()) <= DegenerateAreaSquaredTolerance)
		{
			return Fail(OutError, FString::Printf(TEXT("Triangle %d is geometrically degenerate."), TriangleIDs[DenseTriangle].GetValue()));
		}

		int32 Sorted[3] = { Indices[0], Indices[1], Indices[2] };
		Algo::Sort(Sorted);
		const FIntVector TriangleKey(Sorted[0], Sorted[1], Sorted[2]);
		if (UniqueTriangles.Contains(TriangleKey))
		{
			return Fail(OutError, FString::Printf(TEXT("Triangle %d duplicates an existing triangle."), TriangleIDs[DenseTriangle].GetValue()));
		}
		UniqueTriangles.Add(TriangleKey);
		Baked->TriangleIndices.Append(Indices, UE_ARRAY_COUNT(Indices));

		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const int32 From = Indices[Edge];
			const int32 To = Indices[(Edge + 1) % 3];
			TArray<FEdgeUse>& Uses = EdgeUses.FindOrAdd(MakeEdgeKey(From, To));
			Uses.Add({ DenseTriangle, static_cast<int8>(From < To ? 1 : -1) });
			if (Uses.Num() > 2)
			{
				return Fail(OutError, TEXT("LOD0 is non-manifold: an edge has more than two incident triangles."));
			}
		}
	}

	for (const TPair<uint64, TArray<FEdgeUse>>& Pair : EdgeUses)
	{
		const TArray<FEdgeUse>& Uses = Pair.Value;
		if (Uses.Num() == 2)
		{
			if (Uses[0].Direction == Uses[1].Direction)
			{
				return Fail(OutError, TEXT("LOD0 is not consistently orientable: a shared edge has equal winding."));
			}
			TriangleAdjacency[Uses[0].Triangle].Add(Uses[1].Triangle);
			TriangleAdjacency[Uses[1].Triangle].Add(Uses[0].Triangle);
		}
	}

	// An edge-manifold can still contain a bow-tie vertex. Each vertex link must be
	// one connected cycle (interior) or one connected chain (boundary).
	for (int32 Vertex = 0; Vertex < VertexIDs.Num(); ++Vertex)
	{
		TSet<int32> IncidentTriangles;
		TMap<int32, TArray<int32>> LinkAdjacency;
		int32 BoundaryEdges = 0;
		for (const TPair<uint64, TArray<FEdgeUse>>& Pair : EdgeUses)
		{
			const int32 EdgeA = static_cast<int32>(Pair.Key >> 32);
			const int32 EdgeB = static_cast<int32>(Pair.Key & 0xffffffffull);
			if (EdgeA != Vertex && EdgeB != Vertex)
			{
				continue;
			}
			for (const FEdgeUse& Use : Pair.Value)
			{
				IncidentTriangles.Add(Use.Triangle);
				LinkAdjacency.FindOrAdd(Use.Triangle);
			}
			if (Pair.Value.Num() == 1)
			{
				++BoundaryEdges;
			}
			else
			{
				LinkAdjacency.FindOrAdd(Pair.Value[0].Triangle).Add(Pair.Value[1].Triangle);
				LinkAdjacency.FindOrAdd(Pair.Value[1].Triangle).Add(Pair.Value[0].Triangle);
			}
		}
		if (BoundaryEdges != 0 && BoundaryEdges != 2)
		{
			return Fail(OutError, FString::Printf(TEXT("Source vertex %d has a non-manifold boundary link."), Baked->SourceVertexIDs[Vertex]));
		}
		if (!IncidentTriangles.IsEmpty())
		{
			TArray<int32> LinkStack;
			TSet<int32> LinkVisited;
			LinkStack.Add(*IncidentTriangles.CreateConstIterator());
			while (!LinkStack.IsEmpty())
			{
				const int32 Triangle = LinkStack.Pop(EAllowShrinking::No);
				if (LinkVisited.Contains(Triangle))
				{
					continue;
				}
				LinkVisited.Add(Triangle);
				LinkStack.Append(LinkAdjacency.FindChecked(Triangle));
			}
			if (LinkVisited.Num() != IncidentTriangles.Num())
			{
				return Fail(OutError, FString::Printf(TEXT("Source vertex %d has a disconnected (bow-tie) link."), Baked->SourceVertexIDs[Vertex]));
			}
		}
	}

	TArray<int32> Stack;
	TBitArray<> Visited(false, TriangleIDs.Num());
	Stack.Add(0);
	Visited[0] = true;
	while (!Stack.IsEmpty())
	{
		const int32 Triangle = Stack.Pop(EAllowShrinking::No);
		for (const int32 Neighbor : TriangleAdjacency[Triangle])
		{
			if (!Visited[Neighbor])
			{
				Visited[Neighbor] = true;
				Stack.Add(Neighbor);
			}
		}
	}
	if (Visited.CountSetBits() != TriangleIDs.Num())
	{
		return Fail(OutError, TEXT("LOD0 must be one edge-connected triangle component."));
	}

	TArray<FVertexInstanceID> InstanceIDs;
	for (const FVertexInstanceID InstanceID : Mesh->VertexInstances().GetElementIDs())
	{
		InstanceIDs.Add(InstanceID);
	}
	InstanceIDs.Sort([](FVertexInstanceID A, FVertexInstanceID B) { return A.GetValue() < B.GetValue(); });
	TMap<FVertexInstanceID, int32> DenseBindingByID;
	Baked->RenderBindings.Reserve(InstanceIDs.Num());
	for (const FVertexInstanceID InstanceID : InstanceIDs)
	{
		DenseBindingByID.Add(InstanceID, Baked->RenderBindings.Num());
		FTARibbonRenderVertexBinding& Binding = Baked->RenderBindings.AddDefaulted_GetRef();
		Binding.SimulationVertexIndex = DenseVertex.FindChecked(Mesh->GetVertexInstanceVertex(InstanceID));
		Binding.SourceVertexInstanceID = InstanceID.GetValue();
		Binding.UV0 = UVs.Get(InstanceID, 0);
		Binding.TangentSign = BinormalSigns[InstanceID] < 0.0f ? -1.0f : 1.0f;

		TArray<FTriangleID> ConnectedTriangles(Mesh->GetVertexInstanceConnectedTriangleIDs(InstanceID));
		ConnectedTriangles.Sort([](FTriangleID A, FTriangleID B) { return A.GetValue() < B.GetValue(); });
		if (ConnectedTriangles.IsEmpty())
		{
			return Fail(OutError, FString::Printf(TEXT("Vertex instance %d is not used by a triangle."), InstanceID.GetValue()));
		}
		Binding.PhysicalTriangleIndex = DenseTriangleByID.FindChecked(ConnectedTriangles[0]);
		const TArrayView<const FVertexInstanceID> Corners = Mesh->GetTriangleVertexInstances(ConnectedTriangles[0]);
		if (Corners[0] == InstanceID) Binding.Barycentric = FVector3f(1, 0, 0);
		else if (Corners[1] == InstanceID) Binding.Barycentric = FVector3f(0, 1, 0);
		else if (Corners[2] == InstanceID) Binding.Barycentric = FVector3f(0, 0, 1);
		else return Fail(OutError, TEXT("Internal error: vertex-instance triangle incidence is inconsistent."));
		if (!FMath::IsFinite(Binding.UV0.X) || !FMath::IsFinite(Binding.UV0.Y))
		{
			return Fail(OutError, FString::Printf(TEXT("Vertex instance %d has a non-finite UV0."), InstanceID.GetValue()));
		}
	}
	Baked->RenderIndices.Reserve(TriangleIDs.Num() * 3);
	for (const FTriangleID TriangleID : TriangleIDs)
	{
		for (const FVertexInstanceID InstanceID : Mesh->GetTriangleVertexInstances(TriangleID))
		{
			Baked->RenderIndices.Add(DenseBindingByID.FindChecked(InstanceID));
		}
	}

	if (!ComputeTopologySignature(*Mesh, Baked->TopologySignature, OutError))
	{
		return false;
	}
	Baked->SimulationVertexCount = Baked->RestPositions.Num();
	Baked->TriangleCount = Baked->TriangleIndices.Num() / 3;
	Baked->RenderVertexCount = Baked->RenderBindings.Num();
	Baked->Status = ETARibbonMeshBakeStatus::Valid;

	if (!Baked->IsInternallyConsistent())
	{
		return Fail(OutError, TEXT("Internal error: baked data failed consistency validation."));
	}

	StaticMesh->Modify();
	StaticMesh->RemoveUserDataOfClass(UTARibbonMeshData::StaticClass());
	UTARibbonMeshData* Attached = NewObject<UTARibbonMeshData>(StaticMesh, NAME_None, RF_Transactional);
	Attached->BakeVersion = Baked->BakeVersion;
	Attached->Status = Baked->Status;
	Attached->SimulationVertexCount = Baked->SimulationVertexCount;
	Attached->TriangleCount = Baked->TriangleCount;
	Attached->RenderVertexCount = Baked->RenderVertexCount;
	Attached->SourceVertexIDs = MoveTemp(Baked->SourceVertexIDs);
	Attached->RestPositions = MoveTemp(Baked->RestPositions);
	Attached->PinnedVertices = MoveTemp(Baked->PinnedVertices);
	Attached->WindResponse = MoveTemp(Baked->WindResponse);
	Attached->Permeability = MoveTemp(Baked->Permeability);
	Attached->TriangleIndices = MoveTemp(Baked->TriangleIndices);
	Attached->RenderBindings = MoveTemp(Baked->RenderBindings);
	Attached->RenderIndices = MoveTemp(Baked->RenderIndices);
	Attached->TopologySignature = Baked->TopologySignature;
	StaticMesh->AddAssetUserData(Attached);
	StaticMesh->MarkPackageDirty();
	return true;
}

#endif // WITH_EDITOR
