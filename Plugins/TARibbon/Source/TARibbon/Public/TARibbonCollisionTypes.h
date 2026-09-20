#pragma once

#include "CoreMinimal.h"
#include "TARibbonCollisionTypes.generated.h"

UENUM(BlueprintType)
enum class ETARibbonColliderShape : uint8
{
	Sphere UMETA(DisplayName = "球体"),
	Capsule UMETA(DisplayName = "胶囊"),
	Plane UMETA(DisplayName = "平面（单侧）")
};

/** Validated world geometry, in UE centimeters. Shared by collision sampling and editor drawing. */
struct FTARibbonColliderGeometry
{
	ETARibbonColliderShape Shape = ETARibbonColliderShape::Sphere;
	FVector CenterCm = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	double RadiusCm = 50.0;
	double SegmentHalfLengthCm = 50.0;
	double PlanePreviewExtentCm = 100.0;
};

namespace UE::TARibbon
{
	inline constexpr int32 MaxColliderCount = 64;

	/** Rejects invalid input rather than silently fitting a different collision shape. */
	TARIBBON_API bool BuildColliderGeometry(ETARibbonColliderShape Shape, const FTransform& Transform,
		double RadiusCm, double CapsuleHalfHeightCm, double PlanePreviewExtentCm,
		FTARibbonColliderGeometry& OutGeometry, FString& OutError);
}
