#pragma once

#include "TARibbonCollisionTypes.h"
#include "RibbonGpu.h"

struct FTARibbonColliderPoseSample
{
	double TimeSeconds = 0.0;
	FVector CenterCm = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
};

/** Value-only trajectory: safe to retain in a render-thread request. */
struct FTARibbonColliderTrajectory
{
	FTARibbonColliderGeometry Geometry;
	TArray<FTARibbonColliderPoseSample> Samples;
};

struct FTARibbonCollisionSnapshot
{
	TArray<FTARibbonColliderTrajectory> Colliders;
	int32 InvalidColliderCount = 0;
	FString Warning;
};

namespace UE::TARibbon
{
	bool SameColliderDimensions(const FTARibbonColliderGeometry& A, const FTARibbonColliderGeometry& B);
	void RecordColliderPose(FTARibbonColliderTrajectory& Trajectory, const FTARibbonColliderGeometry& Geometry,
		double TimeSeconds, double RetainAfterSeconds, bool bReset);
	taribbon::Collider SampleCollider(const FTARibbonColliderTrajectory& Trajectory, double TimeSeconds);
	/** Always writes all five arrays, padded to MaxColliderCount, including the zero-contact path. */
	bool UpdateCollisionUpload(taribbon::gpu::Upload& Upload, const FTARibbonCollisionSnapshot* Snapshot,
		double TimeSeconds, double Friction, FString* OutError = nullptr);
}
