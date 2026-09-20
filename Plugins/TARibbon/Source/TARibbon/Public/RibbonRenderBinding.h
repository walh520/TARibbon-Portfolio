#pragma once
#include "Core/RibbonCore.h"

#ifndef TARIBBON_API
#define TARIBBON_RENDER_API
#else
#define TARIBBON_RENDER_API TARIBBON_API
#endif

namespace taribbon {

// Double-precision UE coordinate boundary; the solver frame translates but never rotates.
struct CoordinateFrame {
    Vec3 originUEcm{};
    Vec3 ToSimulation(Vec3 uePositionCm) const { return (uePositionCm-originUEcm)*0.01; }
    Vec3 ToUnreal(Vec3 simulationPositionM) const { return originUEcm+simulationPositionM*100.0; }
    // Pass this to RibbonState::RebaseOrigin, then set originUEcm=newOriginUEcm.
    Vec3 OriginDeltaMeters(Vec3 newOriginUEcm) const { return (newOriginUEcm-originUEcm)*0.01; }
};

struct RenderBinding {
    uint32_t triangle=0;
    Vec3 barycentric{1,0,0};
    double normalOffset=0; // m, rest face normal; independent of render UV
    double tangentSign=1; // render UV handedness; importer supplies this
};
struct SurfaceFrame { Vec3 position{}, normal{0,0,1}, tangent{1,0,0}; double tangentSign=1; };

// Explicit face selection avoids accidental binding across overlapping garments,
// holes, folded layers or disconnected pieces. Reject points outside that triangle.
TARIBBON_RENDER_API bool BindToTriangle(const CookedMesh&, uint32_t triangle,
                                       Vec3 renderRestPosition, RenderBinding& out,
                                       double barycentricTolerance=1e-6);
// Geometric vertex normal gather; a single vertex belongs to one physical chart.
// Render hard seams can use split bindings; no blind welding across UV seams.
TARIBBON_RENDER_API std::vector<SurfaceFrame> BuildSimulationFrames(
    const CookedMesh&, const std::vector<Vec3>& positions);
TARIBBON_RENDER_API SurfaceFrame EvaluateRenderBinding(const CookedMesh&, const RenderBinding&,
    const std::vector<Vec3>& positions, const std::vector<SurfaceFrame>& simulationFrames);
}
