#include "Core/RibbonCore.h"
#include "RibbonGpu.h"
#include "TARibbonComponent.h"
#include "TARibbonMeshData.h"
#include "TARibbonRuntimePrivate.h"
#include "Wind.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <algorithm>
#include <cmath>

namespace UE::TARibbon::Tests
{
using namespace taribbon;

static bool MakeSmallSheet(CookedMesh& OutMesh, FString& OutError)
{
	CookInput Input;
	Input.restPositions = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 }, { 1.0, 1.0, 0.0 }
	};
	Input.materialCoordinates = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 1.0 } };
	Input.triangles = { { 0, 1, 2 }, { 2, 1, 3 } };
	Input.fiberDirections = { { 1.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 } };
	std::string Error;
	if (!CookMesh(Input, OutMesh, nullptr, &Error))
	{
		OutError = UTF8_TO_TCHAR(Error.c_str());
		return false;
	}
	return true;
}

static bool MakeNonPlanarSheet(CookedMesh& OutMesh, FString& OutError)
{
	CookInput Input;
	Input.restPositions = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 }, { 1.0, 1.0, 0.25 }
	};
	Input.materialCoordinates = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 1.0 } };
	Input.triangles = { { 0, 1, 2 }, { 1, 0, 3 } };
	Input.fiberDirections = { { 1.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 } };
	std::string Error;
	if (!CookMesh(Input, OutMesh, nullptr, &Error))
	{
		OutError = UTF8_TO_TCHAR(Error.c_str());
		return false;
	}
	return true;
}

static SimulationConfig MakeConfig()
{
	SimulationConfig Config;
	Config.fixedDt = 1.0 / 60.0;
	Config.substeps = 8;
	Config.iterations = 2;
	Config.maxTicksPerAdvance = 8;
	Config.material.dampingRate = 1.1;
	return Config;
}

static bool IsFinite(const RibbonState& State)
{
	for (const Vec3& Position : State.positions)
	{
		if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
		{
			return false;
		}
	}
	for (const Vec3& Velocity : State.velocities)
	{
		if (!std::isfinite(Velocity.x) || !std::isfinite(Velocity.y) || !std::isfinite(Velocity.z))
		{
			return false;
		}
	}
	return true;
}

static double MaxPositionDelta(const RibbonState& A, const RibbonState& B)
{
	double MaxDeltaSquared = 0.0;
	for (size_t Index = 0; Index < A.positions.size(); ++Index)
	{
		MaxDeltaSquared = std::max(MaxDeltaSquared, LengthSq(A.positions[Index] - B.positions[Index]));
	}
	return std::sqrt(MaxDeltaSquared);
}

static UTARibbonMeshData* MakeValidBakeV2Data()
{
	UTARibbonMeshData* Data = NewObject<UTARibbonMeshData>();
	Data->BakeVersion = UTARibbonMeshData::CurrentBakeVersion;
	Data->Status = ETARibbonMeshBakeStatus::Valid;
	Data->SimulationVertexCount = 4;
	Data->TriangleCount = 2;
	Data->RenderVertexCount = 4;
	Data->SourceVertexIDs = { 0, 1, 2, 3 };
	Data->RestPositions = {
		FVector3f(0, 0, 0), FVector3f(100, 0, 0),
		FVector3f(0, 100, 0), FVector3f(100, 100, 25)
	};
	Data->PinnedVertices = { 1, 1, 0, 0 };
	Data->WindResponse = { 1.0f, 0.5f, 0.0f, 0.25f };
	Data->Permeability = { 0.0f, 0.25f, 0.5f, 1.0f };
	Data->TriangleIndices = { 0, 1, 2, 2, 1, 3 };
	Data->RenderBindings.SetNum(4);
	for (int32 Vertex = 0; Vertex < 4; ++Vertex)
	{
		Data->RenderBindings[Vertex].SimulationVertexIndex = Vertex;
		Data->RenderBindings[Vertex].SourceVertexInstanceID = Vertex;
		Data->RenderBindings[Vertex].PhysicalTriangleIndex = Vertex < 3 ? 0 : 1;
	}
	Data->RenderIndices = { 0, 1, 2, 2, 1, 3 };
	Data->TopologySignature = 1;
	return Data;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonRestNoDriftTest,
	"TA.Ribbon.Core.RestNoDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonRestNoDriftTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook small sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	RibbonState State(Mesh);
	SimulationConfig Config = MakeConfig();
	Config.gravity = {};
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		State.StepFixed(Config, Tick * Config.fixedDt);
	}
	for (size_t Index = 0; Index < State.positions.size(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("Vertex %llu remains at rest"), static_cast<uint64>(Index)),
			Length(State.positions[Index] - Mesh.restPositions[Index]) <= 1.0e-10);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonHardPinGravityTest,
	"TA.Ribbon.Core.HardPinGravityDrop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonHardPinGravityTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook small sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	RibbonState State(Mesh);
	SimulationConfig Config = MakeConfig();
	const std::vector<Pin> Pins = {
		{ 0, Mesh.restPositions[0], Mesh.restPositions[0], true },
		{ 1, Mesh.restPositions[1], Mesh.restPositions[1], true }
	};
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		State.StepFixed(Config, Tick * Config.fixedDt, {}, Pins);
	}
	TestTrue(TEXT("Pin 0 stays exact"), Length(State.positions[0] - Mesh.restPositions[0]) <= 1.0e-12);
	TestTrue(TEXT("Pin 1 stays exact"), Length(State.positions[1] - Mesh.restPositions[1]) <= 1.0e-12);
	TestTrue(TEXT("At least one free vertex drops under gravity"), State.positions[2].z < -1.0e-5 || State.positions[3].z < -1.0e-5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonNonPlanarRestInvariantTest,
	"TA.Ribbon.Core.NonPlanarRestAndHingeUpload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonNonPlanarRestInvariantTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook non-planar rest mesh"), MakeNonPlanarSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("Non-planar mesh has one hinge"), Mesh.hinges.size(), size_t(1));
	if (Mesh.hinges.size() != 1)
	{
		return false;
	}
	for (const TriangleRest& Triangle : Mesh.triangles)
	{
		const TriangleEvaluation Evaluation = EvaluateTriangle(Triangle, Mesh.restPositions);
		TestTrue(TEXT("Non-planar rest triangle is strain-free"), Evaluation.valid &&
			std::abs(Evaluation.cu) <= 1.0e-10 && std::abs(Evaluation.cv) <= 1.0e-10 &&
			std::abs(Evaluation.cs) <= 1.0e-10);
	}
	const HingeEvaluation RestHinge = EvaluateHinge(Mesh.hinges[0], Mesh.restPositions);
	TestTrue(TEXT("Non-planar rest hinge angle is non-zero"), std::abs(Mesh.hinges[0].restAngle) > 1.0e-4);
	TestTrue(TEXT("Non-planar rest hinge angle is retained"), RestHinge.valid && std::abs(RestHinge.constraint) <= 1.0e-10);

	const SimulationConfig Config = MakeConfig();
	RenderBinding Binding;
	Binding.triangle = 1;
	Binding.barycentric = { 0.2, 0.3, 0.5 };
	Binding.normalOffset = 0.025;
	Binding.tangentSign = -1.0;
	const std::vector<SurfaceFrame> RestFrames = BuildSimulationFrames(Mesh, Mesh.restPositions);
	const SurfaceFrame ExpectedRenderFrame = EvaluateRenderBinding(Mesh, Binding, Mesh.restPositions, RestFrames);
	const taribbon::gpu::Upload Upload = taribbon::gpu::BuildInitialUpload(Mesh, Config, { Binding });
	const taribbon::gpu::Float4 RenderPosition = Upload.floats.at("RenderPosition").front();
	const taribbon::gpu::Float4 RenderNormal = Upload.floats.at("RenderNormal").front();
	const taribbon::gpu::Float4 RenderTangent = Upload.floats.at("RenderTangent").front();
	TestTrue(TEXT("Non-planar render binding restores rest position"),
		Length(Vec3{ RenderPosition.x, RenderPosition.y, RenderPosition.z } - ExpectedRenderFrame.position) <= 1.0e-6);
	TestTrue(TEXT("Non-planar render binding restores rest normal"),
		Length(Vec3{ RenderNormal.x, RenderNormal.y, RenderNormal.z } - ExpectedRenderFrame.normal) <= 1.0e-6);
	TestTrue(TEXT("Non-planar render binding restores rest tangent"),
		Length(Vec3{ RenderTangent.x, RenderTangent.y, RenderTangent.z } - ExpectedRenderFrame.tangent) <= 1.0e-6);
	TestTrue(TEXT("Non-planar render binding preserves tangent sign"),
		std::abs(static_cast<double>(RenderTangent.w) - Binding.tangentSign) <= 1.0e-6);
	const taribbon::gpu::Float4 UploadedHinge = Upload.floats.at("HingeRest").front();
	TestTrue(TEXT("GPU HingeRest stores baked rest angle"), std::abs(static_cast<double>(UploadedHinge.x) - Mesh.hinges[0].restAngle) <= 1.0e-6);
	TestTrue(TEXT("GPU HingeRest stores edge length"), std::abs(static_cast<double>(UploadedHinge.y) - Mesh.hinges[0].edgeLength) <= 1.0e-6);
	TestTrue(TEXT("GPU HingeRest stores dual width"), std::abs(static_cast<double>(UploadedHinge.z) - Mesh.hinges[0].dualWidth) <= 1.0e-6);
	const taribbon::gpu::UInt4 UploadedIndices = Upload.uints.at("HingeIndices").front();
	TestEqual(TEXT("GPU HingeIndices stores p0"), UploadedIndices.x, Mesh.hinges[0].p0);
	TestEqual(TEXT("GPU HingeIndices stores p1"), UploadedIndices.y, Mesh.hinges[0].p1);
	TestEqual(TEXT("GPU HingeIndices stores p2"), UploadedIndices.z, Mesh.hinges[0].p2);
	TestEqual(TEXT("GPU HingeIndices stores p3"), UploadedIndices.w, Mesh.hinges[0].p3);

	std::vector<Vec3> Rigid = Mesh.restPositions;
	for (Vec3& Position : Rigid)
	{
		const double X = Position.x;
		const double Y = Position.y;
		Position = { -Y + 4.0, X - 2.0, Position.z + 3.0 };
	}
	for (const TriangleRest& Triangle : Mesh.triangles)
	{
		const TriangleEvaluation Evaluation = EvaluateTriangle(Triangle, Rigid);
		TestTrue(TEXT("Non-planar strain is rigid-motion invariant"), Evaluation.valid &&
			std::abs(Evaluation.cu) <= 1.0e-10 && std::abs(Evaluation.cv) <= 1.0e-10 &&
			std::abs(Evaluation.cs) <= 1.0e-10);
	}
	const HingeEvaluation RigidHinge = EvaluateHinge(Mesh.hinges[0], Rigid);
	TestTrue(TEXT("Non-planar rest angle is rigid-motion invariant"), RigidHinge.valid && std::abs(RigidHinge.constraint) <= 1.0e-10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonTopologyContractTest,
	"TA.Ribbon.Core.TopologyAcceptanceAndRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonTopologyContractTest::RunTest(const FString& Parameters)
{
	using namespace taribbon;
	CookInput Input;
	Input.restPositions = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 }, { 1.0, 1.0, 0.0 }
	};
	Input.materialCoordinates.resize(Input.restPositions.size());
	CookedMesh Mesh;
	std::string Error;

	Input.triangles = { { 0, 1, 2 }, { 2, 1, 3 } };
	TestTrue(TEXT("Planar connected sheet remains accepted"), CookMesh(Input, Mesh, nullptr, &Error));

	Input.triangles = { { 0, 1, 2 }, { 1, 2, 3 } };
	TestFalse(TEXT("Equal winding on a shared edge is rejected"), CookMesh(Input, Mesh, nullptr, &Error));
	TestTrue(TEXT("Winding failure is explicit"), Error.find("winding") != std::string::npos || Error.find("orientation") != std::string::npos);

	Input.restPositions = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 },
		{ 3.0, 0.0, 0.0 }, { 4.0, 0.0, 0.0 }, { 3.0, 1.0, 0.0 }
	};
	Input.materialCoordinates.resize(Input.restPositions.size());
	Input.triangles = { { 0, 1, 2 }, { 3, 4, 5 } };
	TestFalse(TEXT("Disconnected triangle components are rejected"), CookMesh(Input, Mesh, nullptr, &Error));
	TestTrue(TEXT("Connectivity failure is explicit"), Error.find("connected") != std::string::npos);

	Input.restPositions = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 },
		{ 1.0, 1.0, 0.0 }, { 0.5, -1.0, 0.25 }
	};
	Input.materialCoordinates.resize(Input.restPositions.size());
	Input.triangles = { { 0, 1, 2 }, { 1, 0, 3 }, { 0, 1, 4 } };
	TestFalse(TEXT("More than two faces on one edge are rejected"), CookMesh(Input, Mesh, nullptr, &Error));
	TestTrue(TEXT("Non-manifold failure is explicit"), Error.find("manifold") != std::string::npos);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonFiniteStateTest,
	"TA.Ribbon.Core.FiniteState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonFiniteStateTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook small sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	RibbonState State(Mesh);
	const SimulationConfig Config = MakeConfig();
	for (int32 Tick = 0; Tick < 240; ++Tick)
	{
		State.StepFixed(Config, Tick * Config.fixedDt);
		if (!IsFinite(State))
		{
			AddError(FString::Printf(TEXT("Non-finite state after fixed tick %d"), Tick + 1));
			return false;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonFixedTickCadenceInvariantTest,
	"TA.Ribbon.Core.FixedTickCadenceInvariant30_60_120",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonFixedTickCadenceInvariantTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook small sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	SimulationConfig Config = MakeConfig();
	Config.maxTicksPerAdvance = 8;
	RibbonState At30Hz(Mesh);
	RibbonState At60Hz(Mesh);
	RibbonState At120Hz(Mesh);
	for (int32 Frame = 0; Frame < 30; ++Frame) At30Hz.Advance(1.0 / 30.0, Config, Frame / 30.0);
	for (int32 Frame = 0; Frame < 60; ++Frame) At60Hz.Advance(1.0 / 60.0, Config, Frame / 60.0);
	for (int32 Frame = 0; Frame < 120; ++Frame) At120Hz.Advance(1.0 / 120.0, Config, Frame / 120.0);

	TestEqual(TEXT("30 Hz host produces 60 fixed ticks"), At30Hz.diagnostics.fixedTicks, uint64(60));
	TestEqual(TEXT("60 Hz host produces 60 fixed ticks"), At60Hz.diagnostics.fixedTicks, uint64(60));
	TestEqual(TEXT("120 Hz host produces 60 fixed ticks"), At120Hz.diagnostics.fixedTicks, uint64(60));
	TestEqual(TEXT("No 30 Hz dropped ticks"), At30Hz.diagnostics.droppedTicks, uint64(0));
	TestTrue(TEXT("30/60 host cadence yields the same fixed-tick state"), MaxPositionDelta(At30Hz, At60Hz) <= 1.0e-10);
	TestTrue(TEXT("60/120 host cadence yields the same fixed-tick state"), MaxPositionDelta(At60Hz, At120Hz) <= 1.0e-10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonBakeV2MaskContractTest,
	"TA.Ribbon.Asset.BakeV2AndAeroMasks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonBakeV2MaskContractTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	UTARibbonMeshData* Data = MakeValidBakeV2Data();
	TestTrue(TEXT("Bake v2 RGB arrays are internally consistent"), Data->IsInternallyConsistent());

	Data->BakeVersion = 1;
	TestFalse(TEXT("Bake v1 is stale"), Data->IsInternallyConsistent());
	Data->BakeVersion = UTARibbonMeshData::CurrentBakeVersion;
	Data->PinnedVertices = { 0, 0, 0, 0 };
	TestFalse(TEXT("No hard pin is rejected"), Data->IsInternallyConsistent());
	Data->PinnedVertices = { 1, 1, 1, 1 };
	TestFalse(TEXT("All hard pins are rejected"), Data->IsInternallyConsistent());
	Data->PinnedVertices = { 1, 1, 0, 0 };
	Data->WindResponse[2] = 1.01f;
	TestFalse(TEXT("Wind response outside [0,1] is rejected"), Data->IsInternallyConsistent());
	Data->WindResponse[2] = 0.0f;
	Data->Permeability[3] = -0.01f;
	TestFalse(TEXT("Permeability outside [0,1] is rejected"), Data->IsInternallyConsistent());
	Data->Permeability[3] = 1.0f;

	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook mask test sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	taribbon::gpu::Upload Upload = taribbon::gpu::BuildInitialUpload(Mesh, MakeConfig());
	if (!TestTrue(TEXT("Apply painted face masks"), UE::TARibbon::Private::ApplyPaintedAeroMasks(*Data, Upload, &Error)))
	{
		AddError(Error);
		return false;
	}
	const taribbon::gpu::Float4 First = Upload.floats.at("TriRest4")[0];
	const taribbon::gpu::Float4 Second = Upload.floats.at("TriRest4")[1];
	TestTrue(TEXT("First face averages vertex G"), FMath::IsNearlyEqual(First.x, 0.5f));
	TestTrue(TEXT("First face averages vertex B"), FMath::IsNearlyEqual(First.y, 0.25f));
	TestTrue(TEXT("Second face averages vertex G"), FMath::IsNearlyEqual(Second.x, 0.25f));
	TestTrue(TEXT("Second face averages vertex B"), FMath::IsNearlyEqual(Second.y, 7.0f / 12.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonWindUploadAndTimeContractTest,
	"TA.Ribbon.Wind.UploadAndFixedSubstepTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonWindUploadAndTimeContractTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon::Tests;
	CookedMesh Mesh;
	FString Error;
	if (!TestTrue(TEXT("Cook wind contract sheet"), MakeSmallSheet(Mesh, Error)))
	{
		AddError(Error);
		return false;
	}
	SimulationConfig Config = MakeConfig();
	Config.material.windAccelerationClamp = 18.0;
	Config.artWind.waves.push_back({ { 0.0, 1.0, 0.0 }, { 1.0, 0.0 }, 2.5, 0.75, 3.0, 1.25 });
	taribbon::gpu::Upload Upload = taribbon::gpu::BuildInitialUpload(Mesh, Config);
	Upload.parameters.Params1.y = 7.0f;
	TestTrue(TEXT("Acceleration clamp reaches Params2.w"), FMath::IsNearlyEqual(Upload.parameters.Params2.w, 18.0f));
	TestTrue(TEXT("Wind-speed clamp reaches Params1.y"), FMath::IsNearlyEqual(Upload.parameters.Params1.y, 7.0f));
	TestTrue(TEXT("ArtWind is enabled with one wave"),
		FMath::IsNearlyEqual(Upload.parameters.ArtWindParams.x, 1.0f) &&
		FMath::IsNearlyEqual(Upload.parameters.ArtWindParams.y, 1.0f));

	const double WorldTime = 100.0;
	const double StartTime = 20.0;
	const uint32 TickCount = 3;
	const double Offset = UE::TARibbon::Private::ComputeSceneWindTimeOffset(
		WorldTime, StartTime, TickCount, Config.fixedDt);
	const std::vector<taribbon::gpu::Command> LastPlan = taribbon::gpu::BuildTickPlan(
		Mesh, Config, StartTime + static_cast<double>(TickCount - 1) * Config.fixedDt);
	double FirstSample = 0.0;
	double LastSample = 0.0;
	for (const taribbon::gpu::Command& Command : LastPlan)
	{
		if (Command.kind == taribbon::gpu::CommandKind::SampleExternalInputs)
		{
			if (FirstSample == 0.0)
			{
				FirstSample = Command.sampleTime + Offset;
			}
			LastSample = Command.sampleTime + Offset;
		}
	}
	TestTrue(TEXT("Final executed substep aligns to world time"), FMath::IsNearlyEqual(LastSample, WorldTime, 1.0e-10));
	TestTrue(TEXT("Substeps sample distinct times"), LastSample > FirstSample);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonSceneWindSharedShaderContractTest,
	"TA.Ribbon.Wind.SharedSceneWindShaderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonSceneWindSharedShaderContractTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> SceneWindPlugin = IPluginManager::Get().FindPlugin(TEXT("SceneWind"));
	const TSharedPtr<IPlugin> TARibbonPlugin = IPluginManager::Get().FindPlugin(TEXT("TARibbon"));
	if (!TestTrue(TEXT("SceneWind plugin is discoverable"), SceneWindPlugin.IsValid()) ||
		!TestTrue(TEXT("TARibbon plugin is discoverable"), TARibbonPlugin.IsValid()))
	{
		return false;
	}

	FString SharedSource;
	FString SceneWindSource;
	FString RibbonSource;
	const FString SharedPath = FPaths::Combine(SceneWindPlugin->GetBaseDir(), TEXT("Shaders/Private/SceneWindFieldCommon.ush"));
	const FString SceneWindPath = FPaths::Combine(SceneWindPlugin->GetBaseDir(), TEXT("Shaders/Private/SceneWindField.usf"));
	const FString RibbonPath = FPaths::Combine(TARibbonPlugin->GetBaseDir(), TEXT("Shaders/Private/TARibbonSceneWind.usf"));
	TestTrue(TEXT("Load shared SceneWind formula"), FFileHelper::LoadFileToString(SharedSource, *SharedPath));
	TestTrue(TEXT("Load SceneWind field shader"), FFileHelper::LoadFileToString(SceneWindSource, *SceneWindPath));
	TestTrue(TEXT("Load TARibbon adapter shader"), FFileHelper::LoadFileToString(RibbonSource, *RibbonPath));
	TestTrue(TEXT("Shared file defines the canonical evaluator"), SharedSource.Contains(TEXT("SceneWind_EvaluateVelocityCmS")));
	TestTrue(TEXT("SceneWind texture generation calls the canonical evaluator"), SceneWindSource.Contains(TEXT("SceneWind_EvaluateVelocityCmS")));
	TestTrue(TEXT("TARibbon substep adapter calls the canonical evaluator"), RibbonSource.Contains(TEXT("SceneWind_EvaluateVelocityCmS")));
	TestTrue(TEXT("TARibbon adapter reads current positions"), RibbonSource.Contains(TEXT("State0[Triangle.x]")));
	TestTrue(TEXT("TARibbon adapter converts meters to centimeters"), RibbonSource.Contains(TEXT("CentroidMeters.xy * 100.0f")));
	TestTrue(TEXT("TARibbon adapter converts centimeters per second to meters per second"), RibbonSource.Contains(TEXT("VelocityCmS * 0.01f")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonSceneWindCpuReferenceModesTest,
	"TA.Ribbon.Wind.SceneWindCpuReferenceModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonSceneWindCpuReferenceModesTest::RunTest(const FString& Parameters)
{
	FSceneWindFieldEvaluationParameters Wind;
	Wind.BoundsCenter = FVector2f::ZeroVector;
	Wind.BoundsExtent = FVector2f(5000.0f, 5000.0f);
	Wind.BaseDirection = FVector2f(0.6f, 0.8f);
	Wind.BaseSpeed = 500.0f;
	Wind.GustStrength = 0.0f;
	Wind.NoiseStrength = 0.0f;
	Wind.UniformMicroTurbulenceStrength = 0.0f;
	Wind.WindMode = static_cast<uint32>(ESceneWindFieldMode::DirectionalUniform);
	TestTrue(TEXT("Normalized SceneWind snapshot is valid"), Wind.IsValid());
	const FVector2f UniformVelocity = UE::SceneWind::EvaluateVelocityCmS(FVector2f(125.0f, -80.0f), 3.0f, Wind);
	TestTrue(TEXT("Uniform CPU reference matches the canonical analytic result"),
		UniformVelocity.Equals(FVector2f(300.0f, 400.0f), 1.0e-3f));

	Wind.WindMode = static_cast<uint32>(ESceneWindFieldMode::DirectionalWave);
	Wind.WaveAmplitude = 0.5f;
	Wind.WaveSideStrength = 0.15f;
	const FVector2f WaveAtStart = UE::SceneWind::EvaluateVelocityCmS(FVector2f(125.0f, -80.0f), 0.0f, Wind);
	const FVector2f WaveLater = UE::SceneWind::EvaluateVelocityCmS(FVector2f(125.0f, -80.0f), 0.75f, Wind);
	TestTrue(TEXT("Wave mode is finite"), FMath::IsFinite(WaveAtStart.X) && FMath::IsFinite(WaveAtStart.Y));
	TestTrue(TEXT("Wave mode changes with substep time"), !WaveAtStart.Equals(WaveLater, 1.0e-3f));

	Wind.WindMode = static_cast<uint32>(ESceneWindFieldMode::RandomTurbulence);
	const FVector2f RandomVelocity = UE::SceneWind::EvaluateVelocityCmS(FVector2f(125.0f, -80.0f), 0.75f, Wind);
	TestTrue(TEXT("Random-turbulence mode is finite"),
		FMath::IsFinite(RandomVelocity.X) && FMath::IsFinite(RandomVelocity.Y));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTARibbonClothPresetContractTest,
	"TA.Ribbon.Component.ClothPresets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonClothPresetContractTest::RunTest(const FString& Parameters)
{
	UTARibbonComponent* Component = NewObject<UTARibbonComponent>();
	Component->WindSourceMode = ETARibbonWindSourceMode::Disabled;
	Component->BoundsExpansion = 77.0f;
	Component->bEnableArtWind = true;
	Component->ArtWindWaves.AddDefaulted();

	Component->SetClothPreset(ETARibbonClothPreset::Cotton);
	TestTrue(TEXT("Cotton applies density"), FMath::IsNearlyEqual(Component->ArealDensity, 0.18f));
	TestTrue(TEXT("Cotton applies U stretch"), FMath::IsNearlyEqual(Component->StretchStiffnessU, 2200.0f));
	TestTrue(TEXT("Cotton applies V stretch"), FMath::IsNearlyEqual(Component->StretchStiffnessV, 1800.0f));
	TestTrue(TEXT("Cotton applies shear"), FMath::IsNearlyEqual(Component->ShearStiffness, 650.0f));
	TestTrue(TEXT("Cotton applies bend"), FMath::IsNearlyEqual(Component->BendStiffness, 0.0045f));
	TestTrue(TEXT("Cotton applies normal drag"), FMath::IsNearlyEqual(Component->NormalDrag, 1.18f));
	TestTrue(TEXT("Cotton applies tangent drag"), FMath::IsNearlyEqual(Component->TangentDrag, 0.14f));
	TestFalse(TEXT("A material preset disables stale ArtWind"), Component->bEnableArtWind);
	TestEqual(TEXT("A material preset clears stale ArtWind waves"), Component->ArtWindWaves.Num(), 0);
	TestTrue(TEXT("Preset preserves wind-source routing"), Component->WindSourceMode == ETARibbonWindSourceMode::Disabled);
	TestTrue(TEXT("Preset preserves placement settings"), FMath::IsNearlyEqual(Component->BoundsExpansion, 77.0f));

	Component->SetClothPreset(ETARibbonClothPreset::Silk);
	TestTrue(TEXT("Silk applies density"), FMath::IsNearlyEqual(Component->ArealDensity, 0.065f));
	TestTrue(TEXT("Silk applies stretch"), FMath::IsNearlyEqual(Component->StretchStiffnessU, 780.0f));
	TestTrue(TEXT("Silk applies shear"), FMath::IsNearlyEqual(Component->ShearStiffness, 155.0f));
	TestTrue(TEXT("Silk applies bend"), FMath::IsNearlyEqual(Component->BendStiffness, 0.00055f));
	TestTrue(TEXT("Silk applies damping"), FMath::IsNearlyEqual(Component->DampingRate, 0.55f));

	Component->SetClothPreset(ETARibbonClothPreset::Canvas);
	TestTrue(TEXT("Canvas is heavier than cotton"), Component->ArealDensity > 0.18f);
	TestTrue(TEXT("Canvas is stiffer in stretch than cotton"), Component->StretchStiffnessU > 2200.0f);
	TestTrue(TEXT("Canvas is stiffer in bending than cotton"), Component->BendStiffness > 0.0045f);
	TestTrue(TEXT("All authored presets restore fixed-step timing"), FMath::IsNearlyEqual(Component->FixedTimeStep, 1.0f / 60.0f));
	TestEqual(TEXT("All authored presets restore substeps"), Component->Substeps, 8);
	TestEqual(TEXT("All authored presets restore iterations"), Component->SolverIterations, 2);
	TestEqual(TEXT("All authored presets restore catch-up limit"), Component->MaxTicksPerFrame, 4);

	Component->ArealDensity = 0.123f;
	Component->SetClothPreset(ETARibbonClothPreset::Custom);
	TestTrue(TEXT("Custom preserves the current values"), FMath::IsNearlyEqual(Component->ArealDensity, 0.123f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
