#include "Rendering/TARibbonShaders.h"

IMPLEMENT_GLOBAL_SHADER(
	FTARibbonSampleSceneWindCS,
	"/Plugin/TARibbon/Private/TARibbonSceneWind.usf",
	"SampleSceneWind",
	SF_Compute);

#define TARIBBON_IMPLEMENT_COMPUTE_SHADER(ShaderClass, EntryPoint) \
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, "/Plugin/TARibbon/Private/RibbonKernels.usf", EntryPoint, SF_Compute)

TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonClearDiagnosticsCS, "ClearDiagnostics");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonComputeFaceAeroForcesCS, "ComputeFaceAeroForces");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonPredictVerticesCS, "PredictVertices");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonSolveTriangleColorCS, "SolveTriangleColor");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonResetTriangleLambdasCS, "ResetTriangleLambdas");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonResetHingeLambdasCS, "ResetHingeLambdas");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonResetSoftPinLambdasCS, "ResetSoftPinLambdas");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonSolveSoftPinsCS, "SolveSoftPins");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonSolveHingeColorCS, "SolveHingeColor");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonProjectContactsCS, "ProjectContacts");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonUpdateVelocitiesAndApplyContactFrictionCS, "UpdateVelocitiesAndApplyContactFriction");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonGatherVertexFramesCS, "GatherVertexFrames");
TARIBBON_IMPLEMENT_COMPUTE_SHADER(FTARibbonBuildRenderVerticesCS, "BuildRenderVertices");

#undef TARIBBON_IMPLEMENT_COMPUTE_SHADER
