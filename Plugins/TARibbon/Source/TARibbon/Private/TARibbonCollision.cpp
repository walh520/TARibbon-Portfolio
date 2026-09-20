#include "TARibbonCollision.h"

namespace UE::TARibbon
{
namespace
{
bool IsFiniteVector(const FVector& V)
{
	return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
}

bool IsFiniteQuat(const FQuat& Q)
{
	return FMath::IsFinite(Q.X) && FMath::IsFinite(Q.Y) && FMath::IsFinite(Q.Z) && FMath::IsFinite(Q.W);
}

taribbon::Vec3 ToCore(const FVector& V) { return { V.X, V.Y, V.Z }; }
}

bool BuildColliderGeometry(ETARibbonColliderShape Shape, const FTransform& Transform,
	double RadiusCm, double CapsuleHalfHeightCm, double PreviewExtentCm,
	FTARibbonColliderGeometry& OutGeometry, FString& OutError)
{
	OutError.Reset();
	const FVector Scale = Transform.GetScale3D();
	const FQuat Rotation = Transform.GetRotation();
	if (!IsFiniteVector(Transform.GetTranslation()) || !IsFiniteVector(Scale) ||
		!IsFiniteQuat(Rotation) || !FMath::IsFinite(Rotation.SizeSquared()) || Rotation.SizeSquared() <= UE_SMALL_NUMBER)
	{
		OutError = TEXT("碰撞组件变换必须为有限值，并具有有效旋转。");
		return false;
	}
	if (Scale.GetMin() <= 0.0 || !Scale.Equals(FVector(Scale.X), 1.0e-4 * Scale.GetMax()))
	{
		OutError = TEXT("布料碰撞体只支持正的均匀缩放；请调整半径/半高，或使用 Absolute Scale。");
		return false;
	}
	if (Shape != ETARibbonColliderShape::Sphere && Shape != ETARibbonColliderShape::Capsule &&
		Shape != ETARibbonColliderShape::Plane)
	{
		OutError = TEXT("未知的布料碰撞形状。");
		return false;
	}
	if (Shape != ETARibbonColliderShape::Plane && (!FMath::IsFinite(RadiusCm) || RadiusCm <= 0.0))
	{
		OutError = TEXT("球体/胶囊半径必须是大于零的有限值。");
		return false;
	}
	if (Shape == ETARibbonColliderShape::Capsule &&
		(!FMath::IsFinite(CapsuleHalfHeightCm) || CapsuleHalfHeightCm < RadiusCm))
	{
		OutError = TEXT("胶囊半高包含端部半球，必须是有限值且不小于半径。");
		return false;
	}
	if (Shape == ETARibbonColliderShape::Plane && (!FMath::IsFinite(PreviewExtentCm) || PreviewExtentCm <= 0.0))
	{
		OutError = TEXT("平面线框半宽必须是大于零的有限值；实际碰撞平面为无限平面。");
		return false;
	}
	OutGeometry.Shape = Shape;
	OutGeometry.CenterCm = Transform.GetTranslation();
	OutGeometry.Rotation = Rotation.GetNormalized();
	OutGeometry.RadiusCm = Shape == ETARibbonColliderShape::Plane ? 0.0 : RadiusCm * Scale.X;
	OutGeometry.SegmentHalfLengthCm = Shape == ETARibbonColliderShape::Capsule
		? (CapsuleHalfHeightCm - RadiusCm) * Scale.X : 0.0;
	OutGeometry.PlanePreviewExtentCm = Shape == ETARibbonColliderShape::Plane ? PreviewExtentCm * Scale.X : 0.0;
	if (!FMath::IsFinite(OutGeometry.RadiusCm) || !FMath::IsFinite(OutGeometry.SegmentHalfLengthCm) ||
		!FMath::IsFinite(OutGeometry.PlanePreviewExtentCm))
	{
		OutError = TEXT("缩放后的碰撞尺寸溢出。");
		return false;
	}
	return true;
}

bool SameColliderDimensions(const FTARibbonColliderGeometry& A, const FTARibbonColliderGeometry& B)
{
	return A.Shape == B.Shape && A.RadiusCm == B.RadiusCm && A.SegmentHalfLengthCm == B.SegmentHalfLengthCm;
}

void RecordColliderPose(FTARibbonColliderTrajectory& Trajectory, const FTARibbonColliderGeometry& Geometry,
	double TimeSeconds, double RetainAfterSeconds, bool bReset)
{
	if (bReset || !SameColliderDimensions(Trajectory.Geometry, Geometry) ||
		(!Trajectory.Samples.IsEmpty() && TimeSeconds < Trajectory.Samples.Last().TimeSeconds))
	{
		Trajectory.Samples.Reset();
	}
	Trajectory.Geometry = Geometry;
	const FTARibbonColliderPoseSample Sample{ TimeSeconds, Geometry.CenterCm, Geometry.Rotation };
	if (!Trajectory.Samples.IsEmpty() && TimeSeconds == Trajectory.Samples.Last().TimeSeconds)
	{
		Trajectory.Samples.Last() = Sample;
	}
	else
	{
		Trajectory.Samples.Add(Sample);
	}
	// Retain one sample before the window so its first substep can still interpolate.
	int32 RemoveCount = 0;
	while (RemoveCount + 1 < Trajectory.Samples.Num() &&
		Trajectory.Samples[RemoveCount + 1].TimeSeconds < RetainAfterSeconds)
	{
		++RemoveCount;
	}
	if (RemoveCount > 0)
	{
		Trajectory.Samples.RemoveAt(0, RemoveCount, EAllowShrinking::No);
	}
}

taribbon::Collider SampleCollider(const FTARibbonColliderTrajectory& Trajectory, double TimeSeconds)
{
	const FTARibbonColliderGeometry& Geometry = Trajectory.Geometry;
	FVector CenterCm = Geometry.CenterCm;
	FQuat Rotation = Geometry.Rotation;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocity = FVector::ZeroVector;
	const TArray<FTARibbonColliderPoseSample>& Samples = Trajectory.Samples;
	if (!Samples.IsEmpty())
	{
		const FTARibbonColliderPoseSample& Nearest = TimeSeconds < Samples[0].TimeSeconds ? Samples[0] : Samples.Last();
		CenterCm = Nearest.CenterCm;
		Rotation = Nearest.Rotation;
		// The offset and substep sums can straddle an endpoint by roundoff only.
		constexpr double EndpointTimeTolerance = 1.0e-8;
		if (Samples.Num() > 1 && TimeSeconds >= Samples[0].TimeSeconds - EndpointTimeTolerance &&
			TimeSeconds <= Samples.Last().TimeSeconds + EndpointTimeTolerance)
		{
			int32 EndIndex = 1;
			while (EndIndex + 1 < Samples.Num() && Samples[EndIndex].TimeSeconds < TimeSeconds) { ++EndIndex; }
			const FTARibbonColliderPoseSample& A = Samples[EndIndex - 1];
			const FTARibbonColliderPoseSample& B = Samples[EndIndex];
			const double Dt = B.TimeSeconds - A.TimeSeconds;
			if (Dt > UE_DOUBLE_SMALL_NUMBER)
			{
				const double Alpha = FMath::Clamp((TimeSeconds - A.TimeSeconds) / Dt, 0.0, 1.0);
				CenterCm = FMath::Lerp(A.CenterCm, B.CenterCm, Alpha);
				Rotation = FQuat::Slerp(A.Rotation, B.Rotation, Alpha).GetNormalized();
				LinearVelocity = (B.CenterCm - A.CenterCm) * (0.01 / Dt);
				FQuat Delta = (B.Rotation * A.Rotation.Inverse()).GetNormalized();
				if (Delta.W < 0.0) { Delta = FQuat(-Delta.X, -Delta.Y, -Delta.Z, -Delta.W); }
				const FVector Imaginary(Delta.X, Delta.Y, Delta.Z);
				const double SinHalfAngle = Imaginary.Size();
				if (SinHalfAngle > UE_DOUBLE_SMALL_NUMBER)
				{
					const double Angle = 2.0 * FMath::Atan2(SinHalfAngle, Delta.W);
					AngularVelocity = Imaginary * (Angle / (SinHalfAngle * Dt));
				}
			}
		}
	}
	const taribbon::Vec3 Center = ToCore(CenterCm * 0.01);
	const taribbon::Vec3 Linear = ToCore(LinearVelocity);
	const taribbon::Vec3 Angular = ToCore(AngularVelocity);
	taribbon::Collider Result;
	switch (Geometry.Shape)
	{
	case ETARibbonColliderShape::Sphere:
		Result.type = taribbon::Collider::Type::Sphere;
		Result.sphere = { Center, Geometry.RadiusCm * 0.01, Linear, Angular };
		break;
	case ETARibbonColliderShape::Capsule:
	{
		Result.type = taribbon::Collider::Type::Capsule;
		const FVector Axis = Rotation.GetAxisZ() * Geometry.SegmentHalfLengthCm;
		Result.capsule = { ToCore((CenterCm - Axis) * 0.01), ToCore((CenterCm + Axis) * 0.01),
			Geometry.RadiusCm * 0.01, Linear, Angular };
		break;
	}
	case ETARibbonColliderShape::Plane:
		Result.type = taribbon::Collider::Type::Plane;
		Result.plane = { Center, ToCore(Rotation.GetAxisZ()), Linear, Angular };
		break;
	}
	return Result;
}

bool UpdateCollisionUpload(taribbon::gpu::Upload& Upload, const FTARibbonCollisionSnapshot* Snapshot,
	double TimeSeconds, double Friction, FString* OutError)
{
	if ((Snapshot && Snapshot->Colliders.Num() > MaxColliderCount) || !FMath::IsFinite(TimeSeconds) ||
		!FMath::IsFinite(Friction) || Friction < 0.0)
	{
		if (OutError) { *OutError = TEXT("Invalid collision sample or more than 64 active cloth colliders."); }
		return false;
	}
	std::vector<taribbon::Collider> Colliders;
	if (Snapshot)
	{
		Colliders.reserve(Snapshot->Colliders.Num());
		for (const FTARibbonColliderTrajectory& Trajectory : Snapshot->Colliders)
		{
			Colliders.push_back(SampleCollider(Trajectory, TimeSeconds));
		}
	}
	taribbon::gpu::UpdateColliderInputs(Upload, Colliders, Friction);
	for (const char* Name : { "ColliderMeta", "ColliderGeometry0", "ColliderGeometry1", "ColliderMotion0", "ColliderMotion1" })
	{
		auto& Values = Upload.floats.at(Name);
		for (const taribbon::gpu::Float4& Value : Values)
		{
			if (!FMath::IsFinite(Value.x) || !FMath::IsFinite(Value.y) || !FMath::IsFinite(Value.z) || !FMath::IsFinite(Value.w))
			{
				if (OutError) { *OutError = TEXT("Collision geometry or surface velocity overflowed the GPU float ABI."); }
				return false;
			}
		}
		Values.resize(MaxColliderCount, {});
	}
	if (OutError) { OutError->Reset(); }
	return true;
}
}
