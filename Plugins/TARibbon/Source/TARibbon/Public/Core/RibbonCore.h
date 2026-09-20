#pragma once
// Portable TARibbon CPU reference.  The XPBD formulation follows
// https://mmacklin.com/xpbd.pdf ; triangle strains follow
// https://matthias-research.github.io/pages/publications/strainBasedDynamics.pdf .
// Fixed substeps are intentionally used (see https://mmacklin.com/smallsteps.pdf).

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#if defined(TARIBBON_API)
#  define TARIBBON_CORE_API TARIBBON_API
#else
#  define TARIBBON_CORE_API
#endif

namespace taribbon {

constexpr double kEpsilon = 1.0e-12;
constexpr double kPi = 3.1415926535897932384626433832795;

struct Vec2 { double x=0, y=0; };
struct TARIBBON_CORE_API Vec3 {
    double x=0, y=0, z=0;
    Vec3() = default; Vec3(double X,double Y,double Z):x(X),y(Y),z(Z){}
    Vec3& operator+=(const Vec3& b); Vec3& operator-=(const Vec3& b);
    Vec3& operator*=(double s); Vec3& operator/=(double s);
};
TARIBBON_CORE_API Vec3 operator+(Vec3 a, const Vec3& b); TARIBBON_CORE_API Vec3 operator-(Vec3 a, const Vec3& b);
TARIBBON_CORE_API Vec3 operator-(Vec3 a); TARIBBON_CORE_API Vec3 operator*(Vec3 a, double s); TARIBBON_CORE_API Vec3 operator*(double s, Vec3 a);
TARIBBON_CORE_API Vec3 operator/(Vec3 a, double s); TARIBBON_CORE_API double Dot(Vec3 a, Vec3 b); TARIBBON_CORE_API Vec3 Cross(Vec3 a, Vec3 b);
TARIBBON_CORE_API double LengthSq(Vec3 a); TARIBBON_CORE_API double Length(Vec3 a); TARIBBON_CORE_API Vec3 Normalize(Vec3 a, Vec3 fallback={0,0,1});
TARIBBON_CORE_API double Clamp(double value, double lo, double hi); TARIBBON_CORE_API double WrapPi(double radians);

struct TriangleIndex { uint32_t i0=0, i1=0, i2=0; };
struct TriangleRest {
    TriangleIndex indices;
    double area=0;
    // Inverse of Dm. Columns of F are weighted sums using gradFu/gradFv.
    std::array<double,4> invDm{}; // row-major
    std::array<double,3> gradFu{}, gradFv{};
    Vec3 restNormal{0,0,1};
    Vec3 fiberU{1,0,0};
};
struct HingeRest {
    uint32_t p0=0,p1=0,p2=0,p3=0;
    double restAngle=0, edgeLength=0, dualWidth=0;
};

struct CookInput {
    std::vector<Vec3> restPositions;
    std::vector<TriangleIndex> triangles;
    // Optional, one per triangle. It is projected onto each face to choose U.
    std::vector<Vec3> fiberDirections;
    // Optional stable material coordinates for art wind. Defaults to rest x/y.
    std::vector<Vec2> materialCoordinates;
    double arealDensity = 0.2; // kg/m^2
};
struct CookDiagnostics {
    uint32_t fallbackFiberDirections=0;
    std::vector<std::string> warnings;
};
struct CookedMesh {
    std::vector<Vec3> restPositions;
    std::vector<Vec2> materialCoordinates;
    std::vector<TriangleRest> triangles;
    std::vector<HingeRest> hinges;
    std::vector<double> mass, inverseMass;
    std::vector<uint32_t> triangleColors, hingeColors;
    uint32_t triangleColorCount=0, hingeColorCount=0;
};
// Returns false and fills error for any invalid topology/input. Mesh must be a
// consistently oriented two-manifold with boundaries allowed.
TARIBBON_CORE_API bool CookMesh(const CookInput& input, CookedMesh& out, CookDiagnostics* diagnostics=nullptr,
              std::string* error=nullptr);

struct TriangleEvaluation {
    Vec3 Fu{}, Fv{};
    double cu=0, cv=0, cs=0;
    std::array<Vec3,3> gradU{}, gradV{}, gradS{};
    bool valid=false;
};
TARIBBON_CORE_API TriangleEvaluation EvaluateTriangle(const TriangleRest& rest, const std::vector<Vec3>& positions);
struct HingeEvaluation {
    double angle=0, constraint=0;
    std::array<Vec3,4> gradient{}; // d wrapped signed angle / d positions
    bool valid=false;
};
// Analytic chain-rule derivative of the signed atan2 dihedral; no finite
// differences are used by the solver. It retains pre-curved restAngle.
TARIBBON_CORE_API HingeEvaluation EvaluateHinge(const HingeRest& rest, const std::vector<Vec3>& positions);

struct Material {
    double ku=2500, kv=1200, ks=700; // N/m; zero disables the matching scalar constraint
    double bendD=0.002;              // N*m; zero disables bending
    double dampingRate=0.4;          // 1/s
    double thickness=0.002;          // m
    double airDensity=1.225;         // kg/m^3
    double normalDrag=1.1, tangentDrag=0.06;
    double windAccelerationClamp=0;  // m/s^2, 0 disables safeguard
    double contactFriction=0.35;     // Coulomb coefficient for all primitive contacts
};
struct ArtWindWave {
    Vec3 velocityDirection{1,0,0};
    Vec2 materialDirection{1,0};
    double amplitude=0, frequencyHz=0, wavelength=1, phaseSeed=0;
};
struct WindSettings { std::vector<ArtWindWave> waves; };
using WindSample = std::function<Vec3(const Vec3& worldPosition, double simulationTime)>;

struct PlaneCollider { Vec3 point{}, normal{0,0,1}, linearVelocity{}, angularVelocity{}; };
struct SphereCollider { Vec3 center{}; double radius=1; Vec3 linearVelocity{}, angularVelocity{}; };
struct CapsuleCollider { Vec3 a{}, b{0,0,1}; double radius=1; Vec3 linearVelocity{}, angularVelocity{}; };
struct Collider { enum class Type { Plane, Sphere, Capsule } type=Type::Plane; PlaneCollider plane{}; SphereCollider sphere{}; CapsuleCollider capsule{}; };
struct Pin { uint32_t vertex=0; Vec3 targetBegin{}, targetEnd{}; bool enabled=true; };
struct SoftAttachment { uint32_t vertex=0; Vec3 target{}; double compliance=1.0e-6; double influence=1; bool enabled=true; };

struct SimulationConfig {
    double fixedDt=1.0/60.0;
    uint32_t substeps=8, iterations=2, maxTicksPerAdvance=4;
    Vec3 gravity{0,0,-9.81};
    Material material{};
    WindSettings artWind{};
};
TARIBBON_CORE_API void ValidateSimulationConfig(const SimulationConfig& config);

struct Diagnostics {
    uint64_t fixedTicks=0, droppedTicks=0;
    uint32_t skippedTriangleConstraints=0, skippedHinges=0, skippedWindFaces=0, windClamps=0;
    double accumulator=0;
};
class TARIBBON_CORE_API RibbonState {
public:
    explicit RibbonState(const CookedMesh& mesh);
    void Reset();
    void AddExternalDeltaV(uint32_t vertex, Vec3 deltaV); // consumed exactly once next fixed tick
    void SetPinned(uint32_t vertex, bool pinned);
    // Changes the translation-only simulation origin. Argument is newOrigin-oldOrigin in meters.
    // Host must translate pin targets, colliders, wind sample coordinates and render history itself.
    void RebaseOrigin(Vec3 newOriginMinusOldOrigin);
    // Per-vertex values; omitted values use 1 response / 0 permeability.
    std::vector<double> windResponse, permeability;
    std::vector<Vec3> positions, velocities;
    Diagnostics diagnostics;
    // Performs one fixed tick. Pins interpolate begin/end across its substeps.
    void StepFixed(const SimulationConfig&, double time, const WindSample& wind={},
                   const std::vector<Pin>& pins={}, const std::vector<SoftAttachment>& soft={},
                   const std::vector<Collider>& colliders={});
    // Convenience accumulator for static inputs. Moving anchors or time-varying colliders
    // must be resampled by the host and passed to StepFixed for each fixed tick. Dropped
    // full ticks are recorded while fractional time is retained.
    void Advance(double realDt, const SimulationConfig&, double startTime, const WindSample& wind={},
                 const std::vector<Pin>& pins={}, const std::vector<SoftAttachment>& soft={},
                 const std::vector<Collider>& colliders={});
private:
    const CookedMesh* mesh_;
    std::vector<Vec3> deltaV_;
    std::vector<bool> persistentPins_;
    Vec3 originDelta_{};
    double accumulator_=0;
};

} // namespace taribbon

#undef TARIBBON_CORE_API
