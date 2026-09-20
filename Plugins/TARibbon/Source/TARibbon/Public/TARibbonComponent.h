#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "TARibbonComponent.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class UTARibbonMeshData;
class UTARibbonRenderComponent;
class UTARibbonWorldSubsystem;
class AWind;
class FTARibbonGpuSimulation;
struct FTARibbonPreparedSimulation;
struct FTARibbonRenderState;
struct FTARibbonWorldDispatchRequest;
struct FSceneWindFieldEvaluationParameters;
class UTARibbonComponent;

#if WITH_EDITOR
DECLARE_MULTICAST_DELEGATE_TwoParams(FTARibbonEditorPreviewRequestChanged, UTARibbonComponent*, bool);
#endif

UENUM(BlueprintType)
enum class ETARibbonSimulationStatus : uint8
{
	Uninitialized,
	Ready,
	Running,
	Paused,
	Error
};

UENUM(BlueprintType)
enum class ETARibbonWindSourceMode : uint8
{
	ActiveSceneWind UMETA(DisplayName = "Active Scene Wind"),
	SpecificSceneWind UMETA(DisplayName = "Specific Scene Wind"),
	Disabled UMETA(DisplayName = "Disabled")
};

UENUM(BlueprintType)
enum class ETARibbonClothPreset : uint8
{
	Cotton UMETA(DisplayName = "Cotton / 棉布"),
	Silk UMETA(DisplayName = "Silk / 丝绸"),
	Canvas UMETA(DisplayName = "Canvas / 帆布"),
	Custom UMETA(DisplayName = "Custom / 自定义")
};

/** One optional coherent velocity wave. It augments SceneWind before aerodynamic loading. */
USTRUCT(BlueprintType)
struct TARIBBON_API FTARibbonArtWindWave
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ToolTip = "世界空间风速方向"))
	FVector VelocityDirection = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ToolTip = "布料本地 XY 中的波相位方向"))
	FVector2D MaterialDirection = FVector2D(1.0, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ClampMin = "0.0", Units = "m/s", ToolTip = "该艺术风波的速度振幅"))
	float Amplitude = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ClampMin = "0.0", Units = "Hz", ToolTip = "波动频率"))
	float FrequencyHz = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ClampMin = "0.000001", Units = "m", ToolTip = "布料坐标中的波长"))
	float Wavelength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ToolTip = "初始相位，单位为弧度"))
	float PhaseSeed = 0.0f;
};

/**
 * Logic-only TARibbon controller. Attach it below a cloth StaticMeshComponent,
 * or place it on an Actor that has exactly one StaticMeshComponent. The source mesh
 * remains the sole mesh/material authoring source; a private transient renderer
 * replaces its draw only while the GPU simulation is running.
 */
UCLASS(
	ClassGroup = (TA),
	BlueprintType,
	Blueprintable,
	HideCategories = (StaticMesh, Materials, Rendering, Collision, Physics, Navigation, Lighting, LOD, HLOD, RayTracing, TextureStreaming, Mobile),
	meta = (BlueprintSpawnableComponent))
class TARIBBON_API UTARibbonComponent final : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UTARibbonComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual ~UTARibbonComponent() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Preset", meta = (ToolTip = "切换棉布、丝绸或帆布时立即覆盖该预设管理的全部布料、气动与求解参数；手动修改这些参数会自动切换为自定义。"))
	ETARibbonClothPreset ClothPreset = ETARibbonClothPreset::Cotton;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.000001", ToolTip = "布料单位面积质量，kg/m²"))
	float ArealDensity = 0.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "沿本地 U（X）方向的拉伸刚度"))
	float StretchStiffnessU = 2200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "沿本地 V 方向的拉伸刚度"))
	float StretchStiffnessV = 1800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "布料剪切刚度"))
	float ShearStiffness = 650.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "二面角弯曲刚度；数值越小越柔软"))
	float BendStiffness = 0.0045f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "速度阻尼率，s⁻¹"))
	float DampingRate = 1.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.000001", Units = "m", ToolTip = "碰撞与气动力使用的布料厚度"))
	float Thickness = 0.002f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "空气密度，kg/m³"))
	float AirDensity = 1.225f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "法线方向气动阻力系数"))
	float NormalDrag = 1.18f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Cloth", meta = (ClampMin = "0.0", ToolTip = "切线方向气动阻力系数"))
	float TangentDrag = 0.14f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Contacts", meta = (ToolTip = "接收同一 World 中启用的 TA Ribbon 布料碰撞体；开关下一帧生效。"))
	bool bEnableClothCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Contacts", meta = (ClampMin = "0.0", ToolTip = "该布料对全部解析碰撞体的摩擦系数；0 为无摩擦，修改后需要 Reset。"))
	float ContactFriction = 0.35f;

	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "请使用顶点色 B 通道并重新 Bake TARibbon 数据。", ToolTip = "已弃用：渗透率改由顶点色 B 通道控制。"))
	float Permeability_DEPRECATED = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind", meta = (ToolTip = "选择活动 SceneWind、指定风源或禁用 SceneWind。"))
	ETARibbonWindSourceMode WindSourceMode = ETARibbonWindSourceMode::ActiveSceneWind;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind",
		meta = (EditCondition = "WindSourceMode == ETARibbonWindSourceMode::SpecificSceneWind", EditConditionHides, ToolTip = "仅在“指定 SceneWind”模式下使用；必须位于同一 World。"))
	TObjectPtr<AWind> SpecificWindActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind|Safety", meta = (ClampMin = "0.0", Units = "m/s", ToolTip = "合并 SceneWind 与艺术风后的最高风速；0 表示不限制。"))
	float MaxWindSpeed = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind|Safety", meta = (ClampMin = "0.0", Units = "m/s^2", ToolTip = "最高气动加速度；0 表示不限制。"))
	float WindAccelerationClamp = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind|Art", meta = (ToolTip = "启用附加的艺术风波。"))
	bool bEnableArtWind = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Wind|Art",
		meta = (EditCondition = "bEnableArtWind", EditConditionHides, TitleProperty = "FrequencyHz", ToolTip = "最多四条艺术风波；会与 SceneWind 合并后参与气动力计算。"))
	TArray<FTARibbonArtWindWave> ArtWindWaves;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Solver", meta = (ClampMin = "0.0001", Units = "s", ToolTip = "固定物理 Tick 间隔。"))
	float FixedTimeStep = 1.0f / 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Solver", meta = (ClampMin = "1", ClampMax = "64", ToolTip = "每个固定 Tick 的子步数量。"))
	int32 Substeps = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Solver", meta = (ClampMin = "1", ClampMax = "64", ToolTip = "每个子步的约束迭代次数。"))
	int32 SolverIterations = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Solver", meta = (ClampMin = "1", ClampMax = "32", ToolTip = "单帧允许补追的最大固定 Tick 数。"))
	int32 MaxTicksPerFrame = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Placement", meta = (ToolTip = "进入游戏世界后自动开始模拟。"))
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Placement", meta = (ToolTip = "暂停固定 Tick 推进，但仍保留当前渲染结果。"))
	bool bPaused = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Placement", meta = (ClampMin = "0.0", Units = "cm", ToolTip = "在自动 Bounds 外追加的裁剪余量。"))
	float BoundsExpansion = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "异步读取 GPU 诊断计数，会带来少量开销。"))
	bool bReadbackGpuDiagnostics = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "当前模拟状态。"))
	ETARibbonSimulationStatus SimulationStatus = ETARibbonSimulationStatus::Uninitialized;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "最近一次验证、初始化或运行信息。"))
	FString StatusMessage;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "顶点色 R 通道生成的硬固定点数量。"))
	int32 FixedPointCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "参与模拟的物理顶点数量。"))
	int32 SimulationVertexCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "参与模拟的三角形数量。"))
	int32 SimulationTriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "相邻三角形形成的弯曲铰链数量。"))
	int32 SimulationHingeCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "三角形约束图着色数量。"))
	int32 TriangleColorCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "弯曲铰链约束图着色数量。"))
	int32 HingeColorCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "Rest Mesh 中最大的二面角，单位为度。"))
	float MaxRestDihedralDegrees = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "局部 X 投影失败、改用最长边作为 U 方向的三角形数量。"))
	int32 FallbackFiberTriangleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "初态可能因完全对称而保持平面的提示。"))
	bool bInitialPoseSymmetryWarning = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "对称初态的具体原因与处理建议。"))
	FString InitialPoseWarning;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "当前解析到的 SceneWind 参数是否有效。"))
	bool bSceneWindSourceValid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "当前实际使用的 SceneWind Actor。"))
	FString ResolvedWindSourceName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "当前 SceneWind 的场类型。"))
	FString ResolvedSceneWindMode;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "风源缺失、跨 World 或参数无效时的说明。"))
	FString SceneWindWarning;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "因单帧追赶上限而丢弃的固定 Tick 数。"))
	int64 DroppedTicks = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "GPU 检测到的无效三角形数量。"))
	int32 GpuBadTriangles = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "GPU 检测到的无效弯曲铰链数量。"))
	int32 GpuBadHinges = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "风速或气动加速度被限制的累计次数。"))
	int32 GpuWindClamps = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics", meta = (ToolTip = "GPU 检测到的无效接触数量。"))
	int32 GpuBadContacts = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics")
	int32 ActiveColliderCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics")
	int32 InvalidColliderCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "TA Ribbon|Diagnostics")
	FString CollisionWarning;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Preview", meta = (DisplayName = "开始编辑器预览", ToolTip = "在普通关卡编辑视口启动 GPU 布料预览；不会保存模拟形变。"))
	void StartEditorPreview();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Preview", meta = (DisplayName = "停止编辑器预览"))
	void StopEditorPreview();

	UFUNCTION(BlueprintPure, Category = "TA Ribbon|Preview")
	bool IsEditorPreviewActive() const { return bEditorPreviewActive; }

#if WITH_EDITOR
	static FTARibbonEditorPreviewRequestChanged& OnEditorPreviewRequestChanged();
	bool IsEditorPreviewRequested() const { return bEditorPreviewRequested; }
	void SetEditorPreviewSuspended(bool bSuspended);
	bool IsEditorPreviewResetRequired() const { return bResetRequiredAfterEdit || SimulationStatus == ETARibbonSimulationStatus::Error; }
	bool GetEditorPreviewSourceTransform(FTransform& OutTransform) const;
	void RestoreEditorPreviewRequest(bool bRequireReset);
#endif

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Preset", meta = (DisplayName = "应用当前布料预设", ToolTip = "重新应用当前棉布、丝绸或帆布预设的全部托管参数；自定义模式不会改写参数。"))
	void ApplySelectedClothPreset();

	UFUNCTION(BlueprintCallable, Category = "TA Ribbon|Preset", meta = (ToolTip = "选择并立即应用布料预设。"))
	void SetClothPreset(ETARibbonClothPreset NewPreset);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Operations", meta = (ToolTip = "从源 Static Mesh 的 LOD0 生成 TARibbon Bake v2 数据。"))
	void BakeRibbonData();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Operations", meta = (ToolTip = "检查网格、顶点色、材质、Nanite 与组件设置。"))
	void ValidateRibbonSetup();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Operations", meta = (ToolTip = "清除历史并按当前设置重建模拟状态。"))
	void ResetSimulation();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "TA Ribbon|Operations", meta = (ToolTip = "请求推进一个固定物理 Tick。"))
	void StepOneFixedTick();

	UFUNCTION(BlueprintCallable, Category = "TA Ribbon|Operations", meta = (ToolTip = "暂停或恢复固定 Tick 推进。"))
	void SetSimulationPaused(bool bInPaused);

	/** The public component is a controller only and never submits its inherited mesh. */
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	friend class UTARibbonWorldSubsystem;
	friend class UTARibbonRenderComponent;

	UStaticMeshComponent* ResolveSourceMeshComponent(FString* OutError = nullptr) const;
	UStaticMesh* GetSourceStaticMesh() const;
	const UTARibbonMeshData* GetRibbonMeshData() const;
	bool CheckSetup(FString& OutError, bool bCheckFreshness) const;
	bool PrepareSimulation(FString& OutError);
	bool BuildWorldDispatch(float DeltaTime, FTARibbonWorldDispatchRequest& OutRequest);
	void RegisterWithWorldSubsystem();
	void UnregisterFromWorldSubsystem();
	void ReleaseGpuSimulation();
	void SetRuntimeError(const FString& Error);
	void RefreshDiagnostics();
	bool CreateOrRefreshRuntimeRenderer(FString& OutError);
	void DestroyRuntimeRenderer();
	void SuppressSourceRendering(UStaticMeshComponent* SourceComponent);
	void RestoreSourceRendering();
	void RefreshRuntimeRenderState();
	AWind* ResolveSceneWindActor(FString* OutWarning = nullptr) const;
	bool BuildSceneWindSnapshot(FSceneWindFieldEvaluationParameters& OutParameters, FString& OutWarning);
	bool HasEffectiveArtWind() const;
	bool CanSimulateInCurrentWorld() const;
	void StopEditorPreviewState();
	void ApplyClothPresetValues(ETARibbonClothPreset Preset);
	uint32 ComputeResetRequiredConfigurationHash() const;
	void RefreshInputDiagnostics(AWind* ResolvedWindActor, bool bEvaluationParametersValid, const FString& Warning);

	TSharedPtr<FTARibbonPreparedSimulation, ESPMode::ThreadSafe> PreparedSimulation;
	TSharedPtr<FTARibbonGpuSimulation, ESPMode::ThreadSafe> GpuSimulation;
	FTransform SimulationTransform = FTransform::Identity;
	double AccumulatedSeconds = 0.0;
	double SimulatedSeconds = 0.0;
	float AutomaticReachCm = 0.0f;
	float MaxInitialNormalGravityDot = 0.0f;
	uint32 SimulationConfigurationHash = 0;
	uint64 SimulationBakeSignature = 0;
	int32 PendingManualTicks = 0;
	bool bRuntimeStarted = false;
	bool bGpuCreatePending = false;
	bool bRegisteredWithSubsystem = false;

	UPROPERTY(Transient, DuplicateTransient)
	bool bEditorPreviewRequested = false;

	UPROPERTY(Transient, DuplicateTransient)
	bool bEditorPreviewActive = false;

	UPROPERTY(Transient, DuplicateTransient)
	bool bEditorPreviewSuspended = false;
	bool bResetRequiredAfterEdit = false;

	UPROPERTY(Transient)
	TObjectPtr<UTARibbonRenderComponent> RuntimeRenderer;

	TWeakObjectPtr<UStaticMeshComponent> RuntimeSourceComponent;
	TWeakObjectPtr<UStaticMesh> SimulationSourceMesh;
	TWeakObjectPtr<UMaterialInterface> SimulationSourceMaterial;
	bool bSourceRenderingSuppressed = false;
	bool bSourceWasVisible = true;
	bool bSourceWasHiddenInGame = false;
	bool bSourceUsedEditorHiding = false;
	bool bSourceWasTemporarilyHiddenInEditor = false;
};
