#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "TARibbonCollisionTypes.h"
#include "TARibbonColliderComponent.generated.h"

class UTARibbonWorldSubsystem;

/**
 * Authoring-only collider for TARibbon.  The component owns no mesh, material,
 * Chaos body, shadow, or navigation data; the world subsystem consumes its
 * validated geometry and motion-history revision for ribbon collision input.
 */
UCLASS(
	ClassGroup = (TA),
	BlueprintType,
	Blueprintable,
	HideCategories = (Rendering, Collision, Physics, Navigation, Lighting, LOD, HLOD, RayTracing, TextureStreaming, Mobile),
	meta = (BlueprintSpawnableComponent, DisplayName = "TA Ribbon 布料碰撞"))
class TARIBBON_API UTARibbonColliderComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UTARibbonColliderComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Collision shape used by the shared geometry builder and runtime sampler. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "形状", ToolTip = "选择球体、胶囊或单侧无限平面。"))
	ETARibbonColliderShape Shape = ETARibbonColliderShape::Sphere;

	/** Disabled colliders remain registered but are omitted by the world scheduler. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "启用", ToolTip = "关闭后不参与布料碰撞；编辑器仍可显示代理。"))
	bool bEnabled = true;

	/** Sphere radius, or capsule radius, in centimeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "半径（cm）", ClampMin = "0.001",
			EditCondition = "Shape == ETARibbonColliderShape::Sphere || Shape == ETARibbonColliderShape::Capsule", EditConditionHides,
			ToolTip = "球体半径或胶囊半径，单位为厘米。"))
	double RadiusCm = 50.0;

	/** Capsule half-height measured from its center to the end of a hemisphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "胶囊半高（含半球，cm）", ClampMin = "0.001", EditCondition = "Shape == ETARibbonColliderShape::Capsule", EditConditionHides,
			ToolTip = "从胶囊中心到最外端的半高，包含两端半球；必须不小于半径。"))
	double CapsuleHalfHeightCm = 100.0;

	/** Half-side extent for the editor preview square.  Physical plane collision is infinite. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "平面预览半延伸（cm）", ClampMin = "0.001", EditCondition = "Shape == ETARibbonColliderShape::Plane", EditConditionHides,
			ToolTip = "仅用于编辑器有限方框预览；物理碰撞仍是沿本地 +Z 的无限单侧平面。"))
	double PlanePreviewExtentCm = 100.0;

	/** Whether the editor wire proxy should be created and drawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "在编辑器中显示", ToolTip = "在未选中组件时也显示彩色线框代理。"))
	bool bShowInEditor = true;

	/** Diagnostic result written by RefreshColliderValidation. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics",
		meta = (DisplayName = "几何有效"))
	bool bGeometryValid = false;

	/** Shared-builder validation error, or a short valid-state message. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics",
		meta = (DisplayName = "验证信息"))
	FString ValidationMessage;

	/** Builds the exact world-space geometry consumed by both physics and visualization. */
	bool GetColliderGeometry(FTARibbonColliderGeometry& OutGeometry, FString& OutError) const;

	/** Re-evaluates the shared geometry contract and updates the transient diagnostics. */
	void RefreshColliderValidation();

	/** Stable revision used by the world subsystem to preserve motion history. */
	uint64 GetMotionHistoryRevision() const;

	/** Explicitly invalidates the collider's motion history without changing its transform. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Collider",
		meta = (DisplayName = "重置碰撞运动历史", ToolTip = "显式清空该碰撞体的运动历史；重新注册不会自动重置。"))
	void ResetColliderMotionHistory();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport = ETeleportType::None) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	friend class UTARibbonWorldSubsystem;

	/** Deliberately not reset by register/unregister; only the explicit action increments it. */
	uint64 MotionHistoryRevision = 0;

	/** Last state mirrored into the editor proxy; used to avoid per-frame proxy churn. */
	FTARibbonColliderGeometry CachedGeometry;
	FVector CachedFallbackCenter = FVector::ZeroVector;
	bool bCachedGeometryValid = false;
	bool bCachedEnabled = true;
	bool bCachedShowInEditor = true;
	bool bHasCachedProxyState = false;
};
