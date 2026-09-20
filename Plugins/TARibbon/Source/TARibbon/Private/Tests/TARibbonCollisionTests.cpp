#include "TARibbonCollision.h"
#include "TARibbonComponent.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
#include <cmath>
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTARibbonColliderGeometryTest,
	"TA.Ribbon.Collision.GeometryAndUnits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonColliderGeometryTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon;
	FTARibbonColliderGeometry Geometry;
	FString Error;
	FTransform Transform(FQuat::Identity, FVector(100.0, 200.0, 300.0), FVector(2.0));
	TestTrue(TEXT("Valid uniformly scaled sphere"), BuildColliderGeometry(ETARibbonColliderShape::Sphere, Transform, 50, 100, 100, Geometry, Error));
	FTARibbonColliderTrajectory Trajectory;
	RecordColliderPose(Trajectory, Geometry, 0.0, -1.0, true);
	const auto Sphere = SampleCollider(Trajectory, 0.0).sphere;
	TestTrue(TEXT("cm position and radius become meters"), Sphere.center.x == 1.0 && Sphere.center.y == 2.0 && Sphere.center.z == 3.0 && Sphere.radius == 1.0);
	TestTrue(TEXT("Valid capsule"), BuildColliderGeometry(ETARibbonColliderShape::Capsule, Transform, 50, 100, 100, Geometry, Error));
	RecordColliderPose(Trajectory, Geometry, 0.0, -1.0, true);
	const auto Capsule = SampleCollider(Trajectory, 0.0).capsule;
	TestTrue(TEXT("Capsule halfheight includes hemispheres"), Capsule.a.z == 2.0 && Capsule.b.z == 4.0 && Capsule.radius == 1.0);
	Transform.SetRotation(FQuat(FVector::RightVector, UE_DOUBLE_PI / 2.0));
	TestTrue(TEXT("Valid rotated plane"), BuildColliderGeometry(ETARibbonColliderShape::Plane, Transform, 50, 100, 100, Geometry, Error));
	RecordColliderPose(Trajectory, Geometry, 0.0, -1.0, true);
	const auto Plane = SampleCollider(Trajectory, 0.0).plane;
	TestTrue(TEXT("Plane allowed-side normal follows local +Z"), FMath::IsNearlyEqual(Plane.normal.x, 1.0, 1e-10));
	Transform.SetScale3D(FVector(1, 2, 1));
	TestFalse(TEXT("Nonuniform scale rejected"), BuildColliderGeometry(ETARibbonColliderShape::Sphere, Transform, 50, 100, 100, Geometry, Error));
	Transform.SetScale3D(FVector(-1.0));
	TestFalse(TEXT("Negative scale rejected"), BuildColliderGeometry(ETARibbonColliderShape::Sphere, Transform, 50, 100, 100, Geometry, Error));
	Transform.SetScale3D(FVector(0.00001, 0.00002, 0.00001));
	TestFalse(TEXT("Tiny but nonuniform scale is also rejected"), BuildColliderGeometry(ETARibbonColliderShape::Sphere, Transform, 50, 100, 100, Geometry, Error));
	Transform.SetScale3D(FVector::OneVector);
	TestFalse(TEXT("Capsule shorter than radius rejected"), BuildColliderGeometry(ETARibbonColliderShape::Capsule, Transform, 50, 25, 100, Geometry, Error));
	TestFalse(TEXT("Nonfinite radius rejected"), BuildColliderGeometry(ETARibbonColliderShape::Sphere, Transform, std::numeric_limits<double>::infinity(), 100, 100, Geometry, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTARibbonColliderTrajectoryTest,
	"TA.Ribbon.Collision.SubstepTrajectories", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonColliderTrajectoryTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon;
	FTARibbonColliderGeometry Geometry;
	FTARibbonColliderTrajectory Trajectory;
	RecordColliderPose(Trajectory, Geometry, 0.0, -1.0, true);
	Geometry.CenterCm = FVector(100, 0, 0);
	Geometry.Rotation = FQuat(FVector::UpVector, UE_DOUBLE_PI / 2.0);
	RecordColliderPose(Trajectory, Geometry, 1.0, -1.0, false);
	const auto Halfway = SampleCollider(Trajectory, 0.5).sphere;
	TestTrue(TEXT("Substep position interpolates"), FMath::IsNearlyEqual(Halfway.center.x, 0.5, 1e-10));
	TestTrue(TEXT("Linear velocity uses recorded seconds"), FMath::IsNearlyEqual(Halfway.linearVelocity.x, 1.0, 1e-10));
	TestTrue(TEXT("Angular velocity is world rad/s"), FMath::IsNearlyEqual(Halfway.angularVelocity.z, UE_DOUBLE_PI / 2.0, 1e-10));
	const auto Outside = SampleCollider(Trajectory, 2.0).sphere;
	TestTrue(TEXT("Outside history holds endpoint without an impulse"), Outside.center.x == 1.0 && taribbon::LengthSq(Outside.linearVelocity) == 0.0 && taribbon::LengthSq(Outside.angularVelocity) == 0.0);
	const auto Before = SampleCollider(Trajectory, -1.0).sphere;
	TestTrue(TEXT("Before history holds first endpoint"), Before.center.x == 0.0 && taribbon::LengthSq(Before.linearVelocity) == 0.0);
	RecordColliderPose(Trajectory, Geometry, 2.0, 1.0, true);
	TestEqual(TEXT("Explicit reset seeds one stationary pose"), Trajectory.Samples.Num(), 1);
	TestTrue(TEXT("Reset velocity is zero"), taribbon::LengthSq(SampleCollider(Trajectory, 2.0).sphere.linearVelocity) == 0.0);
	Geometry.RadiusCm = 80;
	RecordColliderPose(Trajectory, Geometry, 3.0, 1.0, false);
	TestEqual(TEXT("Geometry edits reset history"), Trajectory.Samples.Num(), 1);
	Geometry.CenterCm.X += 10.0;
	RecordColliderPose(Trajectory, Geometry, 3.0, 1.0, false);
	TestEqual(TEXT("Same timestamp updates the pose without a zero-duration segment"), Trajectory.Samples.Num(), 1);
	RecordColliderPose(Trajectory, Geometry, 4.0, 1.0, false);
	RecordColliderPose(Trajectory, Geometry, 5.0, 4.5, false);
	TestTrue(TEXT("Pruning retains the predecessor for substep interpolation"),
		Trajectory.Samples.Num() == 2 && Trajectory.Samples[0].TimeSeconds == 4.0);
	RecordColliderPose(Trajectory, Geometry, 0.0, -1.0, false);
	TestEqual(TEXT("World time reversal resets the history"), Trajectory.Samples.Num(), 1);
	const FQuat EquivalentRotation = Geometry.Rotation;
	Geometry.Rotation = FQuat(-EquivalentRotation.X, -EquivalentRotation.Y, -EquivalentRotation.Z, -EquivalentRotation.W);
	RecordColliderPose(Trajectory, Geometry, 1.0, -1.0, false);
	TestTrue(TEXT("Quaternion sign change does not invent angular velocity"),
		taribbon::LengthSq(SampleCollider(Trajectory, 0.5).sphere.angularVelocity) <= 1e-20);

	for (int32 Fps : { 30, 60, 120 })
	{
		FTARibbonColliderTrajectory Recorded;
		for (int32 Frame = 0; Frame <= Fps; ++Frame)
		{
			const double Time = static_cast<double>(Frame) / Fps;
			Geometry.CenterCm = FVector(Time * 100.0, 0, 0);
			Geometry.Rotation = FQuat(FVector::UpVector, Time * UE_DOUBLE_PI / 2.0);
			RecordColliderPose(Recorded, Geometry, Time, -1.0, Frame == 0);
		}
		for (int32 Substep = 1; Substep <= 480; ++Substep)
		{
			const double Time = static_cast<double>(Substep) / 480.0;
			const auto Sample = SampleCollider(Recorded, Time).sphere;
			TestTrue(TEXT("Same recorded linear trajectory at 30/60/120 host rates"),
				FMath::IsNearlyEqual(Sample.center.x, Time, 1e-10) && FMath::IsNearlyEqual(Sample.linearVelocity.x, 1.0, 1e-10) &&
				FMath::IsNearlyEqual(Sample.angularVelocity.z, UE_DOUBLE_PI / 2.0, 1e-10));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTARibbonColliderUploadTest,
	"TA.Ribbon.Collision.FixedCapacityAndRemoval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonColliderUploadTest::RunTest(const FString& Parameters)
{
	using namespace UE::TARibbon;
	taribbon::gpu::Upload Upload;
	Upload.floats["State0"] = { { 1, 2, 3, 4 } };
	Upload.floats["FaceWind"] = { { 5, 6, 7, 0 } };
	FTARibbonCollisionSnapshot Snapshot;
	FString Error;
	for (int32 Count : { 0, 1, 5, 64, 0 })
	{
		Snapshot.Colliders.SetNum(Count);
		TestTrue(TEXT("Collider count changes fit persistent buffers"), UpdateCollisionUpload(Upload, &Snapshot, 0.0, 0.35, &Error));
		TestEqual(TEXT("Count is active count, not capacity"), taribbon::gpu::ReadUIntBits(Upload.parameters.Params3.w), static_cast<uint32>(Count));
		for (const char* Name : { "ColliderMeta", "ColliderGeometry0", "ColliderGeometry1", "ColliderMotion0", "ColliderMotion1" })
		{
			const auto& Values = Upload.floats.at(Name);
			TestEqual(TEXT("Each collider buffer stays at 64 entries"), static_cast<int32>(Values.size()), 64);
			for (int32 Index = Count; Index < 64; ++Index)
			{
				TestTrue(TEXT("Inactive slots are cleared"), Values[Index].x == 0 && Values[Index].y == 0 && Values[Index].z == 0 && Values[Index].w == 0);
			}
		}
	}
	Snapshot.Colliders.SetNum(65);
	TestFalse(TEXT("Overflow is rejected, never silently truncated"), UpdateCollisionUpload(Upload, &Snapshot, 0.0, 0.35, &Error));
	TestTrue(TEXT("Disabled collision explicitly clears count"), UpdateCollisionUpload(Upload, nullptr, 0.0, 0.35, &Error));
	TestTrue(TEXT("Collision uploads preserve simulation and GPU SceneWind inputs"), Upload.floats.at("State0")[0].x == 1 && Upload.floats.at("FaceWind")[0].x == 5);
	UTARibbonComponent* Component = NewObject<UTARibbonComponent>();
	Component->bEnableClothCollision = false;
	for (ETARibbonClothPreset Preset : { ETARibbonClothPreset::Cotton, ETARibbonClothPreset::Silk, ETARibbonClothPreset::Canvas })
	{
		Component->ContactFriction = 0.9f;
		Component->SetClothPreset(Preset);
		TestEqual(TEXT("Preset restores contact friction"), Component->ContactFriction, 0.35f);
		TestFalse(TEXT("Preset preserves collision routing toggle"), Component->bEnableClothCollision);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTARibbonContactResponseTest,
	"TA.Ribbon.Collision.ProjectionPinsAndFriction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTARibbonContactResponseTest::RunTest(const FString& Parameters)
{
	using namespace taribbon;
	CookInput Input;
	Input.restPositions = { { 0, 0, 0 }, { 0.2, 0, 0 }, { 0, 0.2, 0 } };
	Input.materialCoordinates = { { 0, 0 }, { 0.2, 0 }, { 0, 0.2 } };
	Input.triangles = { { 0, 1, 2 } };
	CookedMesh Mesh;
	std::string Error;
	if (!TestTrue(TEXT("Cook contact reference sheet"), CookMesh(Input, Mesh, nullptr, &Error))) { return false; }
	SimulationConfig Config;
	Config.substeps = 1;
	Config.iterations = 1;
	Config.gravity = {};
	Config.material.ku = Config.material.kv = Config.material.ks = Config.material.bendD = 0.0;
	Config.material.dampingRate = Config.material.airDensity = 0.0;
	Collider Plane;
	Plane.type = Collider::Type::Plane;
	Plane.plane.point = { 0, 0, -Config.material.thickness };
	Plane.plane.normal = { 0, 0, 1 };
	RibbonState Slippery(Mesh), Rough(Mesh);
	for (RibbonState* State : { &Slippery, &Rough })
	{
		State->SetPinned(0, true);
		State->velocities[1] = State->velocities[2] = { 1, 0, -1 };
	}
	Config.material.contactFriction = 0.0;
	Slippery.StepFixed(Config, 0.0, {}, {}, {}, { Plane });
	Config.material.contactFriction = 0.35;
	Rough.StepFixed(Config, 0.0, {}, {}, {}, { Plane });
	TestTrue(TEXT("Fixed vertex unaffected"), LengthSq(Rough.positions[0] - Mesh.restPositions[0]) == 0.0);
	TestTrue(TEXT("Plane separates and removes incoming velocity without bounce"), Rough.positions[1].z >= -1e-10 && FMath::Abs(Rough.velocities[1].z) <= 1e-10);
	TestTrue(TEXT("Contact friction reduces tangential velocity"), Rough.velocities[1].x < Slippery.velocities[1].x && Rough.velocities[1].x >= 0.0);
	for (bool bRotate : { false, true })
	{
		Collider MovingPlane = Plane;
		MovingPlane.plane.point.z += Config.fixedDt;
		MovingPlane.plane.linearVelocity = bRotate ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 1 };
		MovingPlane.plane.angularVelocity = bRotate ? Vec3{ 0, 0, 2 } : Vec3{};
		RibbonState State(Mesh);
		State.SetPinned(0, true);
		State.StepFixed(Config, 0.0, {}, {}, {}, { MovingPlane });
		TestTrue(TEXT("Moving plane transfers normal surface speed without relative bounce"),
			FMath::IsNearlyEqual(State.velocities[1].z, 1.0, 1e-10));
		TestTrue(TEXT("Friction transfers translation or angular surface motion"),
			(bRotate ? State.velocities[1].y : State.velocities[1].x) > 0.0);
		TestTrue(TEXT("Moving contacts never move hard pins"), LengthSq(State.positions[0] - Mesh.restPositions[0]) == 0.0);
	}
	for (Collider::Type Type : { Collider::Type::Sphere, Collider::Type::Capsule })
	{
		Collider Object;
		Object.type = Type;
		Object.sphere = { { 0, 0, -0.1 }, 0.5, {}, {} };
		Object.capsule = { { 0, -1, -0.1 }, { 0, 1, -0.1 }, 0.5, {}, {} };
		RibbonState State(Mesh);
		State.SetPinned(0, true);
		State.StepFixed(Config, 0.0, {}, {}, {}, { Object });
		const Vec3 P = State.positions[1];
		const double Distance = Type == Collider::Type::Sphere ? Length(P - Object.sphere.center) - 0.5
			: std::sqrt(P.x * P.x + (P.z + 0.1) * (P.z + 0.1)) - 0.5;
		TestTrue(TEXT("Sphere/capsule project outside thickness"), Distance >= Config.material.thickness - 1e-10);
		TestTrue(TEXT("Primitive contacts preserve hard pins"), LengthSq(State.positions[0] - Mesh.restPositions[0]) == 0.0);
	}
	return true;
}
#endif
