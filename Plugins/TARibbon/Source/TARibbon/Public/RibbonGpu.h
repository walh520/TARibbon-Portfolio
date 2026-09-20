#pragma once
#include "RibbonRenderBinding.h"
#include <map>
#include <string>

namespace taribbon::gpu {
struct alignas(16) Float4 { float x=0,y=0,z=0,w=0; };
struct alignas(16) UInt4 { uint32_t x=0,y=0,z=0,w=0; };
static_assert(sizeof(Float4)==16 && sizeof(UInt4)==16, "HLSL lane stride mismatch");
struct alignas(16) Parameters {
    Float4 Params0,Params1,Params2,Params3,GravityAndTime,ArtWindParams,PassParams;
};
static_assert(sizeof(Parameters)==112, "RibbonParameters constant-buffer size mismatch");

// Exact HLSL resource names. These are INITIAL upload arrays, not CPU mirrors of
// live GPU state. Keep live GPU state resident; never upload these old positions
// each tick. All empty logical buffers carry one zero element for API binding.
struct Upload {
    Parameters parameters;
    std::map<std::string,std::vector<Float4>> floats;
    std::map<std::string,std::vector<UInt4>> uints;
};
TARIBBON_RENDER_API float UIntBits(uint32_t value);
TARIBBON_RENDER_API uint32_t ReadUIntBits(float value);
TARIBBON_RENDER_API std::vector<UInt4> PackUInts(const std::vector<uint32_t>& scalars);

// Pin targetBegin and targetEnd should both equal the initial target. Hard and
// soft pins may not overlap. Barycentric bindings select a known physical face.
TARIBBON_RENDER_API Upload BuildInitialUpload(const CookedMesh&, const SimulationConfig&,
    const std::vector<RenderBinding>& bindings={}, const std::vector<Pin>& pins={},
    const std::vector<SoftAttachment>& soft={}, const std::vector<Collider>& colliders={});

// Independent per-substep updates; these do not write State0/State1.
TARIBBON_RENDER_API void UpdateColliderInputs(Upload&, const std::vector<Collider>&, double friction);
TARIBBON_RENDER_API void UpdatePinTargetInputs(Upload&, const std::vector<Pin>&,
    const std::vector<SoftAttachment>&, double previousFraction, double currentFraction);

enum class CommandKind { SampleExternalInputs, Dispatch };
struct Command {
    CommandKind kind=CommandKind::Dispatch;
    std::string entryPoint;
    uint32_t groupsX=0, substep=0, iteration=0, color=0;
    double sampleTime=0;
    bool barrierAfter=true;
};
// SampleExternalInputs is a host callback stage (NOT an HLSL entrypoint): sample
// the supplied wind field at current GPU face centroids and interpolate targets.
// Dispatch commands map directly to implemented entrypoints, with explicit
// resource dependencies after every command. Each command owns its constants;
// do not retain a pointer to a reused mutable parameter block in a deferred graph.
TARIBBON_RENDER_API std::vector<Command> BuildTickPlan(const CookedMesh&,
    const SimulationConfig&, double tickStartTime);
TARIBBON_RENDER_API std::vector<Command> BuildRenderPlan(uint32_t simulationVertices,
    uint32_t renderVertices); // once per rendered frame, after zero/more ticks
}
