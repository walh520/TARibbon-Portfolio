#include "TARibbonColliderComponent.h"

#include "TARibbonWorldSubsystem.h"

#include "Engine/World.h"
#include "MeshElementCollector.h"
#include "Math/RotationMatrix.h"
#include "PrimitiveDrawingUtils.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "SceneView.h"

namespace TARibbonColliderComponentPrivate
{
	static bool IsFiniteGeometry(const FTARibbonColliderGeometry& Geometry)
	{
		if (Geometry.CenterCm.ContainsNaN() || Geometry.Rotation.ContainsNaN() || !Geometry.Rotation.IsNormalized())
		{
			return false;
		}

		if (!FMath::IsFinite(Geometry.RadiusCm) || !FMath::IsFinite(Geometry.SegmentHalfLengthCm) ||
			!FMath::IsFinite(Geometry.PlanePreviewExtentCm))
		{
			return false;
		}

		switch (Geometry.Shape)
		{
		case ETARibbonColliderShape::Sphere:
			return Geometry.RadiusCm > 0.0;
		case ETARibbonColliderShape::Capsule:
			return Geometry.RadiusCm > 0.0 && Geometry.SegmentHalfLengthCm >= 0.0;
		case ETARibbonColliderShape::Plane:
			return Geometry.PlanePreviewExtentCm > 0.0;
		default:
			return false;
		}
	}

	static FVector FiniteLocationOr(const FVector& Candidate, const FVector& Fallback = FVector::ZeroVector)
	{
		return Candidate.ContainsNaN() ? Fallback : Candidate;
	}

	static bool GeometryEquals(const FTARibbonColliderGeometry& A, const FTARibbonColliderGeometry& B)
	{
		constexpr double Tolerance = 1.0e-4;
		return A.Shape == B.Shape && A.CenterCm.Equals(B.CenterCm, Tolerance) &&
			A.Rotation.Equals(B.Rotation, Tolerance) &&
			FMath::IsNearlyEqual(A.RadiusCm, B.RadiusCm, Tolerance) &&
			FMath::IsNearlyEqual(A.SegmentHalfLengthCm, B.SegmentHalfLengthCm, Tolerance) &&
			FMath::IsNearlyEqual(A.PlanePreviewExtentCm, B.PlanePreviewExtentCm, Tolerance);
	}

	static FLinearColor GetGeometryColor(bool bGeometryValid, bool bEnabled)
	{
		if (!bGeometryValid)
		{
			return FLinearColor(1.0f, 0.08f, 0.08f, 1.0f);
		}
		return bEnabled
			? FLinearColor(0.12f, 1.0f, 0.22f, 1.0f)
			: FLinearColor(0.52f, 0.52f, 0.52f, 1.0f);
	}

	static void DrawPlanePreview(FPrimitiveDrawInterface* PDI, const FTARibbonColliderGeometry& Geometry,
		const FLinearColor& Color, uint8 DepthPriority, float Thickness)
	{
		const FVector X = Geometry.Rotation.RotateVector(FVector(1.0, 0.0, 0.0)).GetSafeNormal();
		const FVector Y = Geometry.Rotation.RotateVector(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
		const FVector Z = Geometry.Rotation.RotateVector(FVector(0.0, 0.0, 1.0)).GetSafeNormal();
		const FVector Center = Geometry.CenterCm;
		const double Extent = Geometry.PlanePreviewExtentCm;

		const FVector A = Center + (X + Y) * Extent;
		const FVector B = Center + (X - Y) * Extent;
		const FVector C = Center + (-X - Y) * Extent;
		const FVector D = Center + (-X + Y) * Extent;
		PDI->DrawLine(A, B, Color, DepthPriority, Thickness);
		PDI->DrawLine(B, C, Color, DepthPriority, Thickness);
		PDI->DrawLine(C, D, Color, DepthPriority, Thickness);
		PDI->DrawLine(D, A, Color, DepthPriority, Thickness);

		// DrawDirectionalArrow is +X-oriented, so construct a basis whose local X is
		// the plane's local +Z.  The finite square remains preview-only; physics is
		// still an infinite single-sided plane.
		const float ArrowLength = FMath::Max(30.0f, static_cast<float>(Extent * 0.35));
		const float ArrowSize = FMath::Clamp(ArrowLength * 0.25f, 6.0f, 40.0f);
		FMatrix ArrowToWorld = FRotationMatrix::MakeFromXZ(Z, X);
		ArrowToWorld.SetOrigin(Center);
		DrawDirectionalArrow(PDI, ArrowToWorld, Color, ArrowLength, ArrowSize, DepthPriority, Thickness);
	}

	static void DrawInvalidMarker(FPrimitiveDrawInterface* PDI, const FVector& Center,
		const FLinearColor& Color, uint8 DepthPriority, float Thickness)
	{
		constexpr float MarkerRadius = 25.0f;
		DrawWireSphere(PDI, Center, Color, MarkerRadius, 12, DepthPriority, Thickness);
		DrawWireStar(PDI, Center, MarkerRadius, Color, DepthPriority);
	}
}

#if WITH_EDITOR
/** Editor-only wire proxy.  It owns copied values and never dereferences a UObject on the render thread. */
class FTARibbonColliderSceneProxy final : public FPrimitiveSceneProxy
{
public:
	 explicit FTARibbonColliderSceneProxy(const UTARibbonColliderComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, bGeometryValid(false)
		, bEnabled(Component->bEnabled)
		, bShowInEditor(Component->bShowInEditor)
		, Geometry()
		, FallbackCenter(TARibbonColliderComponentPrivate::FiniteLocationOr(Component->GetComponentLocation()))
	{
		FString Error;
		bGeometryValid = Component->GetColliderGeometry(Geometry, Error);
		if (!bGeometryValid || !TARibbonColliderComponentPrivate::IsFiniteGeometry(Geometry))
		{
			bGeometryValid = false;
			Geometry = FTARibbonColliderGeometry();
		}

		bWillEverBeLit = false;
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& /*ViewFamily*/,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		const FLinearColor Color = TARibbonColliderComponentPrivate::GetGeometryColor(bGeometryValid, bEnabled);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1u << ViewIndex)) == 0u)
			{
				continue;
			}

			const FSceneView* View = Views[ViewIndex];
			if (!View || View->bIsGameView)
			{
				continue;
			}

			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
			if (!bGeometryValid)
			{
				TARibbonColliderComponentPrivate::DrawInvalidMarker(PDI, FallbackCenter, Color, SDPG_Foreground, 1.5f);
				continue;
			}

			switch (Geometry.Shape)
			{
			case ETARibbonColliderShape::Sphere:
				DrawWireSphere(PDI, Geometry.CenterCm, Color, Geometry.RadiusCm, 24, SDPG_Foreground, 1.5f);
				break;

			case ETARibbonColliderShape::Capsule:
			{
				const FVector X = Geometry.Rotation.RotateVector(FVector(1.0, 0.0, 0.0)).GetSafeNormal();
				const FVector Y = Geometry.Rotation.RotateVector(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
				const FVector Z = Geometry.Rotation.RotateVector(FVector(0.0, 0.0, 1.0)).GetSafeNormal();
				DrawWireCapsule(PDI, Geometry.CenterCm, X, Y, Z, Color, Geometry.RadiusCm,
					Geometry.RadiusCm + Geometry.SegmentHalfLengthCm, 24, SDPG_Foreground, 1.5f);
				break;
			}

			case ETARibbonColliderShape::Plane:
				TARibbonColliderComponentPrivate::DrawPlanePreview(PDI, Geometry, Color, SDPG_Foreground, 1.5f);
				break;

			default:
				TARibbonColliderComponentPrivate::DrawInvalidMarker(PDI, FallbackCenter, Color, SDPG_Foreground, 1.5f);
				break;
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		const bool bVisible = bShowInEditor && View && !View->bIsGameView && IsShown(View);
		Result.bDrawRelevance = bVisible;
		Result.bDynamicRelevance = bVisible;
		Result.bShadowRelevance = false;
		Result.bEditorPrimitiveRelevance = bVisible && UseEditorCompositing(View);
		Result.bEditorNoDepthTestPrimitiveRelevance = bVisible && UseEditorCompositing(View);
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}

	private:
	bool bGeometryValid;
	bool bEnabled;
	bool bShowInEditor;
	FTARibbonColliderGeometry Geometry;
	FVector FallbackCenter;
};
#endif // WITH_EDITOR

UTARibbonColliderComponent::UTARibbonColliderComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	bWantsOnUpdateTransform = true;
	bUseEditorCompositing = true;

	// This component is a scheduler input and editor wire proxy, not a renderable
	// or Chaos primitive.  Keep all inherited engine-facing participation disabled.
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	SetCastShadow(false);
	SetHiddenInGame(true);
	SetMobility(EComponentMobility::Movable);

	ValidationMessage = TEXT("尚未验证碰撞几何。");
}

bool UTARibbonColliderComponent::GetColliderGeometry(FTARibbonColliderGeometry& OutGeometry, FString& OutError) const
{
	OutGeometry = FTARibbonColliderGeometry();
	OutError.Reset();

	const bool bBuilt = UE::TARibbon::BuildColliderGeometry(
		Shape,
		GetComponentTransform(),
		RadiusCm,
		CapsuleHalfHeightCm,
		PlanePreviewExtentCm,
		OutGeometry,
		OutError);

	if (bBuilt && !TARibbonColliderComponentPrivate::IsFiniteGeometry(OutGeometry))
	{
		OutError = TEXT("Shared collider geometry builder returned non-finite geometry.");
		OutGeometry = FTARibbonColliderGeometry();
		return false;
	}

	if (!bBuilt && OutError.IsEmpty())
	{
		OutError = TEXT("Invalid collider geometry.");
	}
	return bBuilt;
}

void UTARibbonColliderComponent::RefreshColliderValidation()
{
	FTARibbonColliderGeometry Geometry;
	FString Error;
	const bool bNewGeometryValid = GetColliderGeometry(Geometry, Error);
	const FVector NewFallbackCenter = TARibbonColliderComponentPrivate::FiniteLocationOr(GetComponentLocation());
	const bool bProxyStateChanged = bHasCachedProxyState &&
		(bCachedGeometryValid != bNewGeometryValid || bCachedEnabled != bEnabled || bCachedShowInEditor != bShowInEditor ||
			(bNewGeometryValid
				? !TARibbonColliderComponentPrivate::GeometryEquals(CachedGeometry, Geometry)
				: !CachedFallbackCenter.Equals(NewFallbackCenter, 1.0e-4)));

	bGeometryValid = bNewGeometryValid;
	ValidationMessage = bGeometryValid ? TEXT("碰撞几何有效。") : MoveTemp(Error);
	if (ValidationMessage.IsEmpty())
	{
		ValidationMessage = TEXT("碰撞几何无效。");
	}

	CachedGeometry = Geometry;
	CachedFallbackCenter = NewFallbackCenter;
	bCachedGeometryValid = bNewGeometryValid;
	bCachedEnabled = bEnabled;
	bCachedShowInEditor = bShowInEditor;
	bHasCachedProxyState = true;
	if (bProxyStateChanged)
	{
		UpdateBounds();
		if (IsRegistered())
		{
			MarkRenderStateDirty();
		}
	}
}

uint64 UTARibbonColliderComponent::GetMotionHistoryRevision() const
{
	return MotionHistoryRevision;
}

void UTARibbonColliderComponent::ResetColliderMotionHistory()
{
	++MotionHistoryRevision;
}

FPrimitiveSceneProxy* UTARibbonColliderComponent::CreateSceneProxy()
{
#if WITH_EDITOR
	return bShowInEditor ? new FTARibbonColliderSceneProxy(this) : nullptr;
#else
	return nullptr;
#endif
}

FBoxSphereBounds UTARibbonColliderComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FTARibbonColliderGeometry Geometry;
	FString Error;
	const bool bValid = UE::TARibbon::BuildColliderGeometry(
		Shape,
		LocalToWorld,
		RadiusCm,
		CapsuleHalfHeightCm,
		PlanePreviewExtentCm,
		Geometry,
		Error) && TARibbonColliderComponentPrivate::IsFiniteGeometry(Geometry);

	if (!bValid)
	{
		const FVector FallbackCenter = TARibbonColliderComponentPrivate::FiniteLocationOr(LocalToWorld.GetLocation());
		return FBoxSphereBounds(FallbackCenter, FVector(25.0), 25.0);
	}

	const FVector WorldX = Geometry.Rotation.RotateVector(FVector(1.0, 0.0, 0.0)).GetSafeNormal();
	const FVector WorldY = Geometry.Rotation.RotateVector(FVector(0.0, 1.0, 0.0)).GetSafeNormal();
	const FVector WorldZ = Geometry.Rotation.RotateVector(FVector(0.0, 0.0, 1.0)).GetSafeNormal();
	FVector BoxExtent = FVector::ZeroVector;
	float SphereRadius = 0.0f;

	switch (Geometry.Shape)
	{
	case ETARibbonColliderShape::Sphere:
		BoxExtent = FVector(Geometry.RadiusCm);
		SphereRadius = static_cast<float>(Geometry.RadiusCm);
		break;

	case ETARibbonColliderShape::Capsule:
		BoxExtent = FVector(
			Geometry.RadiusCm + Geometry.SegmentHalfLengthCm * FMath::Abs(WorldZ.X),
			Geometry.RadiusCm + Geometry.SegmentHalfLengthCm * FMath::Abs(WorldZ.Y),
			Geometry.RadiusCm + Geometry.SegmentHalfLengthCm * FMath::Abs(WorldZ.Z));
		SphereRadius = static_cast<float>(Geometry.RadiusCm + Geometry.SegmentHalfLengthCm);
		break;

	case ETARibbonColliderShape::Plane:
	{
		const double Extent = Geometry.PlanePreviewExtentCm;
		const float ArrowLength = FMath::Max(30.0f, static_cast<float>(Extent * 0.35));
		const float ArrowSize = FMath::Clamp(ArrowLength * 0.25f, 6.0f, 40.0f);
		const FVector PlaneExtent(
			Extent * (FMath::Abs(WorldX.X) + FMath::Abs(WorldY.X)) + ArrowLength * FMath::Abs(WorldZ.X),
			Extent * (FMath::Abs(WorldX.Y) + FMath::Abs(WorldY.Y)) + ArrowLength * FMath::Abs(WorldZ.Y),
			Extent * (FMath::Abs(WorldX.Z) + FMath::Abs(WorldY.Z)) + ArrowLength * FMath::Abs(WorldZ.Z));
		const FVector ArrowY = (WorldX ^ WorldZ).GetSafeNormal();
		const FVector ArrowZ = WorldZ ^ ArrowY;
		const FVector ArrowExtent(
			FMath::Max(ArrowLength * FMath::Abs(WorldZ.X), (ArrowLength - ArrowSize) * FMath::Abs(WorldZ.X) + ArrowSize * (FMath::Abs(ArrowY.X) + FMath::Abs(ArrowZ.X))),
			FMath::Max(ArrowLength * FMath::Abs(WorldZ.Y), (ArrowLength - ArrowSize) * FMath::Abs(WorldZ.Y) + ArrowSize * (FMath::Abs(ArrowY.Y) + FMath::Abs(ArrowZ.Y))),
			FMath::Max(
				ArrowLength * FMath::Abs(WorldZ.Z),
				(ArrowLength - ArrowSize) * FMath::Abs(WorldZ.Z) + ArrowSize * (FMath::Abs(ArrowY.Z) + FMath::Abs(ArrowZ.Z))));
		BoxExtent = FVector(
			FMath::Max(PlaneExtent.X, ArrowExtent.X),
			FMath::Max(PlaneExtent.Y, ArrowExtent.Y),
			FMath::Max(PlaneExtent.Z, ArrowExtent.Z));
		SphereRadius = BoxExtent.Size();
		break;
	}

	default:
		return FBoxSphereBounds(TARibbonColliderComponentPrivate::FiniteLocationOr(LocalToWorld.GetLocation()), FVector(25.0), 25.0);
	}

	return FBoxSphereBounds(Geometry.CenterCm, BoxExtent, SphereRadius);
}

void UTARibbonColliderComponent::OnRegister()
{
	RefreshColliderValidation();
	Super::OnRegister();

	if (UWorld* World = GetWorld())
	{
		if (UTARibbonWorldSubsystem* Subsystem = World->GetSubsystem<UTARibbonWorldSubsystem>())
		{
			Subsystem->RegisterCollider(this);
		}
	}
}

void UTARibbonColliderComponent::OnUnregister()
{
	if (UWorld* World = GetWorld())
	{
		if (UTARibbonWorldSubsystem* Subsystem = World->GetSubsystem<UTARibbonWorldSubsystem>())
		{
			Subsystem->UnregisterCollider(this);
		}
	}

	Super::OnUnregister();
}

void UTARibbonColliderComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	if (UWorld* World = GetWorld())
	{
		if (UTARibbonWorldSubsystem* Subsystem = World->GetSubsystem<UTARibbonWorldSubsystem>())
		{
			Subsystem->UnregisterCollider(this);
		}
	}

	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UTARibbonColliderComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	RefreshColliderValidation();
}

#if WITH_EDITOR
void UTARibbonColliderComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Construction scripts can replace this component while the base implementation
	// runs.  Finish all accesses to this instance before calling Super.
	RefreshColliderValidation();
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
