#include "Core/RibbonCore.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace taribbon {

Vec3& Vec3::operator+=(const Vec3& b) { x += b.x; y += b.y; z += b.z; return *this; }
Vec3& Vec3::operator-=(const Vec3& b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
Vec3& Vec3::operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
Vec3& Vec3::operator/=(double s) { x /= s; y /= s; z /= s; return *this; }
Vec3 operator+(Vec3 a, const Vec3& b) { return a += b; }
Vec3 operator-(Vec3 a, const Vec3& b) { return a -= b; }
Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
Vec3 operator*(Vec3 a, double s) { return a *= s; }
Vec3 operator*(double s, Vec3 a) { return a *= s; }
Vec3 operator/(Vec3 a, double s) { return a /= s; }
double Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double LengthSq(Vec3 a) { return Dot(a, a); }
double Length(Vec3 a) { return std::sqrt(LengthSq(a)); }
Vec3 Normalize(Vec3 a, Vec3 fallback) { double l = Length(a); return l > kEpsilon ? a/l : fallback; }
double Clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
double WrapPi(double a) { return a-2*kPi*std::floor((a+kPi)/(2*kPi)); }

namespace {
struct EdgeUse { uint32_t start, end, opposite, face; };
using EdgeKey = std::pair<uint32_t,uint32_t>;

EdgeKey MakeEdgeKey(uint32_t a, uint32_t b) { return {std::min(a,b), std::max(a,b)}; }
bool Finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool Finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool Finite(double v) { return std::isfinite(v); }
[[noreturn]] void Invalid(const char* message) { throw std::invalid_argument(message); }
void Require(bool condition, const char* message) { if (!condition) Invalid(message); }
void SetError(std::string* error, const char* message) { if (error) *error = message; }

Vec3 LongestEdge(Vec3 a, Vec3 b, Vec3 c) {
    Vec3 ab=b-a, bc=c-b, ca=a-c;
    if (LengthSq(ab) >= LengthSq(bc) && LengthSq(ab) >= LengthSq(ca)) return ab;
    return LengthSq(bc) >= LengthSq(ca) ? bc : ca;
}

double SignedAngle(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, bool* valid=nullptr) {
    Vec3 e=p1-p0, c0=Cross(e,p2-p0), c1=Cross(p3-p0,e);
    if (Length(e)<=kEpsilon || Length(c0)<=kEpsilon || Length(c1)<=kEpsilon) {
        if (valid) *valid=false;
        return 0;
    }
    if (valid) *valid=true;
    Vec3 eh=Normalize(e), n0=Normalize(c0), n1=Normalize(c1);
    return std::atan2(Dot(eh,Cross(n0,n1)), Dot(n0,n1));
}

uint32_t ColorConstraints(const std::vector<std::vector<uint32_t>>& constraints,
                          size_t vertexCount, std::vector<uint32_t>& colors) {
    // Greedy color selection from colors already present at each incident vertex.
    // This is O(number of constraint-vertex incidences), unlike pairwise scans.
    std::vector<std::unordered_set<uint32_t>> incidentColors(vertexCount);
    colors.resize(constraints.size());
    uint32_t largest=0;
    for (size_t ci=0; ci<constraints.size(); ++ci) {
        std::unordered_set<uint32_t> forbidden;
        for (uint32_t v : constraints[ci])
            forbidden.insert(incidentColors[v].begin(), incidentColors[v].end());
        uint32_t color=0;
        while (forbidden.count(color)) ++color;
        colors[ci]=color;
        largest=std::max(largest,color);
        for (uint32_t v : constraints[ci]) incidentColors[v].insert(color);
    }
    return constraints.empty() ? 0 : largest+1;
}

Vec3 SurfaceVelocity(Vec3 point, Vec3 pivot, Vec3 linear, Vec3 angular) {
    return linear + Cross(angular, point-pivot);
}

void ContactGeometry(const Vec3& point, const Collider& collider,
                     Vec3& normal, Vec3& surfaceVelocity, double& signedDistance) {
    switch (collider.type) {
    case Collider::Type::Plane:
        normal=Normalize(collider.plane.normal);
        signedDistance=Dot(point-collider.plane.point,normal);
        surfaceVelocity=SurfaceVelocity(point,collider.plane.point,
                                        collider.plane.linearVelocity,collider.plane.angularVelocity);
        return;
    case Collider::Type::Sphere: {
        Vec3 q=point-collider.sphere.center;
        normal=Normalize(q,{0,0,1});
        signedDistance=Length(q)-collider.sphere.radius;
        surfaceVelocity=SurfaceVelocity(point,collider.sphere.center,
                                        collider.sphere.linearVelocity,collider.sphere.angularVelocity);
        return;
    }
    case Collider::Type::Capsule: {
        Vec3 axis=collider.capsule.b-collider.capsule.a;
        double t=LengthSq(axis)>1e-24 ? Clamp(Dot(point-collider.capsule.a,axis)/LengthSq(axis),0,1) : 0;
        Vec3 closest=collider.capsule.a+axis*t;
        Vec3 axisUnit=Normalize(axis,{0,0,1});
        Vec3 reference=std::abs(axisUnit.z)<0.8 ? Vec3{0,0,1} : Vec3{0,1,0};
        normal=Normalize(point-closest,Normalize(Cross(reference,axisUnit),{1,0,0}));
        signedDistance=Length(point-closest)-collider.capsule.radius;
        Vec3 midpoint=(collider.capsule.a+collider.capsule.b)*0.5;
        surfaceVelocity=SurfaceVelocity(point,midpoint,
                                        collider.capsule.linearVelocity,collider.capsule.angularVelocity);
        return;
    }
    }
}

void ProjectContact(Vec3& position, double inverseMass, double thickness, const Collider& collider) {
    if (inverseMass == 0) return;
    Vec3 normal, surfaceV; double distance;
    ContactGeometry(position,collider,normal,surfaceV,distance);
    if (distance < thickness) position += normal*(thickness-distance);
}

void ResolveContactVelocity(const Vec3& position, Vec3& velocity, Vec3 freeVelocity,
                            double thickness, double friction, const Collider& collider) {
    Vec3 normal, surfaceV; double distance;
    ContactGeometry(position,collider,normal,surfaceV,distance);
    if (distance > thickness+1.e-7) return;

    Vec3 currentRelative=velocity-surfaceV;
    Vec3 freeRelative=freeVelocity-surfaceV;
    double currentNormal=Dot(currentRelative,normal);
    double freeNormal=Dot(freeRelative,normal);
    // Constraints can make an originally separating vertex incoming. It must still
    // lose that inward component. Separating in both states creates no friction.
    if (currentNormal >= 0 && freeNormal >= 0) return;
    double normalDelta=std::max(0.0,std::max(-currentNormal,-freeNormal));
    if (currentNormal < 0) velocity -= normal*currentNormal;
    Vec3 tangent=currentRelative-normal*currentNormal;
    double tangentLength=Length(tangent);
    if (tangentLength > kEpsilon)
        velocity -= tangent*(std::min(tangentLength,friction*normalDelta)/tangentLength);
}

double FaceAverage(const std::vector<double>& values, TriangleIndex t, double fallback) {
    if (values.size() <= std::max({t.i0,t.i1,t.i2})) return fallback;
    return (values[t.i0]+values[t.i1]+values[t.i2])/3.0;
}

void ProjectScalar(std::vector<Vec3>& x, const std::array<uint32_t,3>& ids,
                   const std::array<Vec3,3>& gradient, const std::vector<double>& inverseMass,
                   double constraint, double alphaTilde, double& lambda) {
    double denominator=alphaTilde;
    for (int i=0;i<3;++i) denominator += inverseMass[ids[i]]*LengthSq(gradient[i]);
    if (denominator <= kEpsilon) return;
    double deltaLambda=(-constraint-alphaTilde*lambda)/denominator;
    lambda += deltaLambda;
    for (int i=0;i<3;++i) x[ids[i]] += gradient[i]*(inverseMass[ids[i]]*deltaLambda);
}

void ProjectHinge(std::vector<Vec3>& x, const HingeRest& hinge, const HingeEvaluation& evaluation,
                  const std::vector<double>& inverseMass, double alphaTilde, double& lambda) {
    const std::array<uint32_t,4> ids={hinge.p0,hinge.p1,hinge.p2,hinge.p3};
    double denominator=alphaTilde;
    for (int i=0;i<4;++i) denominator += inverseMass[ids[i]]*LengthSq(evaluation.gradient[i]);
    if (denominator <= kEpsilon) return;
    double deltaLambda=(-evaluation.constraint-alphaTilde*lambda)/denominator;
    lambda += deltaLambda;
    for (int i=0;i<4;++i) x[ids[i]] += evaluation.gradient[i]*(inverseMass[ids[i]]*deltaLambda);
}
} // namespace

bool CookMesh(const CookInput& input, CookedMesh& out, CookDiagnostics* diagnostics, std::string* error) {
    out={};
    if (diagnostics) *diagnostics={};
    if (input.restPositions.empty() || input.triangles.empty()) { SetError(error,"mesh needs vertices and triangles"); return false; }
    if (!std::isfinite(input.arealDensity) || input.arealDensity<=0) { SetError(error,"arealDensity must be finite and positive"); return false; }
    if (!input.fiberDirections.empty() && input.fiberDirections.size()!=input.triangles.size()) { SetError(error,"fiberDirections must be per triangle"); return false; }
    if (!input.materialCoordinates.empty() && input.materialCoordinates.size()!=input.restPositions.size()) { SetError(error,"materialCoordinates must be per vertex"); return false; }
    for (Vec3 p:input.restPositions) if (!Finite(p)) { SetError(error,"rest position is not finite"); return false; }
    for (Vec3 f:input.fiberDirections) if (!Finite(f)) { SetError(error,"fiber direction is not finite"); return false; }
    for (Vec2 uv:input.materialCoordinates) if (!Finite(uv)) { SetError(error,"material coordinate is not finite"); return false; }

    out.restPositions=input.restPositions;
    out.materialCoordinates=input.materialCoordinates;
    if (out.materialCoordinates.empty()) for (Vec3 p:input.restPositions) out.materialCoordinates.push_back({p.x,p.y});
    out.mass.assign(input.restPositions.size(),0);

    std::set<std::array<uint32_t,3>> uniqueTriangles;
    std::map<EdgeKey,std::vector<EdgeUse>> edges;
    std::vector<std::vector<uint32_t>> vertexFaces(input.restPositions.size());
    for (uint32_t face=0; face<input.triangles.size(); ++face) {
        TriangleIndex t=input.triangles[face];
        if (t.i0>=input.restPositions.size() || t.i1>=input.restPositions.size() || t.i2>=input.restPositions.size() ||
            t.i0==t.i1 || t.i1==t.i2 || t.i2==t.i0) { SetError(error,"triangle has invalid indices"); return false; }
        std::array<uint32_t,3> canonical={t.i0,t.i1,t.i2}; std::sort(canonical.begin(),canonical.end());
        if (!uniqueTriangles.insert(canonical).second) { SetError(error,"duplicate triangle"); return false; }

        Vec3 p0=input.restPositions[t.i0],p1=input.restPositions[t.i1],p2=input.restPositions[t.i2];
        Vec3 rawNormal=Cross(p1-p0,p2-p0); double area=0.5*Length(rawNormal);
        if (!Finite(area) || area<=kEpsilon) { SetError(error,"degenerate or nonfinite rest triangle"); return false; }
        Vec3 normal=Normalize(rawNormal);
        Vec3 fiber=input.fiberDirections.empty()?Vec3{}:input.fiberDirections[face];
        fiber-=normal*Dot(fiber,normal);
        if (LengthSq(fiber)<=kEpsilon) {
            fiber=LongestEdge(p0,p1,p2); fiber-=normal*Dot(fiber,normal);
            if (diagnostics) ++diagnostics->fallbackFiberDirections;
        }
        Vec3 u=Normalize(fiber),v=Normalize(Cross(normal,u));
        Vec3 d1=p1-p0,d2=p2-p0;
        double a=Dot(d1,u), b=Dot(d2,u), c=Dot(d1,v), d=Dot(d2,v), determinant=a*d-b*c;
        if (!Finite(determinant) || std::abs(determinant)<=kEpsilon) { SetError(error,"singular or nonfinite rest chart"); return false; }

        TriangleRest rest;
        rest.indices=t; rest.area=area; rest.restNormal=normal; rest.fiberU=u;
        rest.invDm={d/determinant,-b/determinant,-c/determinant,a/determinant};
        rest.gradFu={-(rest.invDm[0]+rest.invDm[2]),rest.invDm[0],rest.invDm[2]};
        rest.gradFv={-(rest.invDm[1]+rest.invDm[3]),rest.invDm[1],rest.invDm[3]};
        out.triangles.push_back(rest);
        double lump=input.arealDensity*area/3.0;
        out.mass[t.i0]+=lump; out.mass[t.i1]+=lump; out.mass[t.i2]+=lump;
        vertexFaces[t.i0].push_back(face); vertexFaces[t.i1].push_back(face); vertexFaces[t.i2].push_back(face);
        const EdgeUse uses[3]={{t.i0,t.i1,t.i2,face},{t.i1,t.i2,t.i0,face},{t.i2,t.i0,t.i1,face}};
        for (EdgeUse use:uses) {
            auto& list=edges[MakeEdgeKey(use.start,use.end)]; list.push_back(use);
            if (list.size()>2) { SetError(error,"nonmanifold edge"); return false; }
        }
    }

    std::vector<std::vector<uint32_t>> faceNeighbors(out.triangles.size());
    for (const auto& pair:edges) {
        const auto& uses=pair.second;
        if (uses.size()!=2) continue;
        const EdgeUse& a=uses[0]; const EdgeUse& b=uses[1];
        if (a.start!=b.end || a.end!=b.start) { SetError(error,"inconsistent triangle winding across edge"); return false; }
        faceNeighbors[a.face].push_back(b.face); faceNeighbors[b.face].push_back(a.face);
        const TriangleRest& first=out.triangles[a.face]; const TriangleRest& second=out.triangles[b.face];
        double length=Length(input.restPositions[a.end]-input.restPositions[a.start]);
        HingeRest hinge;
        hinge.p0=a.start; hinge.p1=a.end; hinge.p2=a.opposite; hinge.p3=b.opposite;
        hinge.edgeLength=length; hinge.dualWidth=2*(first.area+second.area)/(3*length);
        bool valid=false;
        hinge.restAngle=SignedAngle(input.restPositions[hinge.p0],input.restPositions[hinge.p1],input.restPositions[hinge.p2],input.restPositions[hinge.p3],&valid);
        if (!valid) { SetError(error,"degenerate hinge"); return false; }
        out.hinges.push_back(hinge);
    }

	// V1.2 accepts boundaries and holes, but one simulated sheet must remain a
	// single edge-connected triangle component.
	if (!out.triangles.empty()) {
		std::vector<uint8_t> visitedFaces(out.triangles.size(),0);
		std::vector<uint32_t> stack={0}; visitedFaces[0]=1; size_t visitedCount=0;
		while (!stack.empty()) {
			uint32_t face=stack.back(); stack.pop_back(); ++visitedCount;
			for (uint32_t neighbor:faceNeighbors[face]) if (!visitedFaces[neighbor]) {
				visitedFaces[neighbor]=1; stack.push_back(neighbor);
			}
		}
		if (visitedCount!=out.triangles.size()) { SetError(error,"disconnected triangle components"); return false; }
	}

    // Every incident-face fan at a vertex must be connected. Disconnected fans
    // are bow-tie vertices even if every individual edge has manifold degree.
    for (uint32_t vertex=0;vertex<vertexFaces.size();++vertex) {
        const auto& faces=vertexFaces[vertex];
        if (faces.empty()) { SetError(error,"isolated vertex"); return false; }
        std::unordered_set<uint32_t> allowed(faces.begin(),faces.end()), visited;
        std::vector<uint32_t> stack={faces.front()}; visited.insert(faces.front());
        while (!stack.empty()) {
            uint32_t face=stack.back(); stack.pop_back();
            for (uint32_t neighbor:faceNeighbors[face]) if (allowed.count(neighbor) && visited.insert(neighbor).second) stack.push_back(neighbor);
        }
        if (visited.size()!=faces.size()) { SetError(error,"bow-tie vertex"); return false; }
    }
    out.inverseMass.resize(out.mass.size());
    for (size_t i=0;i<out.mass.size();++i) {
        if(!Finite(out.mass[i]) || out.mass[i]<=0 || !Finite(1.0/out.mass[i])) {
            SetError(error,"nonfinite mass or inverse mass"); return false;
        }
        out.inverseMass[i]=1.0/out.mass[i];
    }

    std::vector<std::vector<uint32_t>> triangleVertices,hingeVertices;
    for (const TriangleRest& t:out.triangles) triangleVertices.push_back({t.indices.i0,t.indices.i1,t.indices.i2});
    for (const HingeRest& h:out.hinges) hingeVertices.push_back({h.p0,h.p1,h.p2,h.p3});
    out.triangleColorCount=ColorConstraints(triangleVertices,out.restPositions.size(),out.triangleColors);
    out.hingeColorCount=ColorConstraints(hingeVertices,out.restPositions.size(),out.hingeColors);
    return true;
}

TriangleEvaluation EvaluateTriangle(const TriangleRest& rest, const std::vector<Vec3>& positions) {
    TriangleEvaluation out;
    TriangleIndex t=rest.indices;
    if (t.i0>=positions.size() || t.i1>=positions.size() || t.i2>=positions.size()) return out;
    Vec3 p0=positions[t.i0], d1=positions[t.i1]-p0, d2=positions[t.i2]-p0;
    double doubleArea=Length(Cross(d1,d2));
    if (doubleArea<=std::max(1.e-12,2.e-6*rest.area)) return out;
    // Edge differences retain small deformations when positions have a large world offset.
    out.Fu=d1*rest.invDm[0]+d2*rest.invDm[2];
    out.Fv=d1*rest.invDm[1]+d2*rest.invDm[3];
    double lu=Length(out.Fu),lv=Length(out.Fv);
    if (lu<=kEpsilon || lv<=kEpsilon) return out;
    out.cu=lu-1; out.cv=lv-1; out.cs=Dot(out.Fu,out.Fv);
    for (int i=0;i<3;++i) {
        out.gradU[i]=out.Fu*(rest.gradFu[i]/lu);
        out.gradV[i]=out.Fv*(rest.gradFv[i]/lv);
        out.gradS[i]=out.Fv*rest.gradFu[i]+out.Fu*rest.gradFv[i];
    }
    out.valid=true;
    return out;
}

HingeEvaluation EvaluateHinge(const HingeRest& rest, const std::vector<Vec3>& x) {
    HingeEvaluation out;
    if (rest.p0>=x.size() || rest.p1>=x.size() || rest.p2>=x.size() || rest.p3>=x.size()) return out;
    Vec3 p0=x[rest.p0],p1=x[rest.p1],p2=x[rest.p2],p3=x[rest.p3],e=p1-p0;
    double length=Length(e),lengthSq=LengthSq(e);
    Vec3 n0=Cross(e,p2-p0),n1=Cross(p3-p0,e);
    double n0Sq=LengthSq(n0),n1Sq=LengthSq(n1);
    double normalFloor=std::max(1.e-24,1.e-12*std::pow(rest.edgeLength,4));
    if (length<=kEpsilon || n0Sq<=normalFloor || n1Sq<=normalFloor) return out;
    bool valid=false; out.angle=SignedAngle(p0,p1,p2,p3,&valid);
    if (!valid) return out;
    out.constraint=WrapPi(out.angle-rest.restAngle);
    // Analytic derivative of the exact signed atan2 convention in the contract.
    Vec3 g2=(-length/n0Sq)*n0, g3=(-length/n1Sq)*n1;
    double s2=Dot(p2-p0,e)/lengthSq,s3=Dot(p3-p0,e)/lengthSq;
    out.gradient[0]=(s2-1)*g2+(s3-1)*g3;
    out.gradient[1]=-s2*g2-s3*g3;
    out.gradient[2]=g2; out.gradient[3]=g3; out.valid=true;
    return out;
}

void ValidateSimulationConfig(const SimulationConfig& config) {
    Require(Finite(config.fixedDt) && config.fixedDt>0,"fixedDt must be finite and positive");
    Require(config.substeps>0 && config.iterations>0 && config.maxTicksPerAdvance>0,"step counts must be positive");
    const Material& m=config.material;
    Require(Finite(m.ku)&&m.ku>=0&&Finite(m.kv)&&m.kv>=0&&Finite(m.ks)&&m.ks>=0&&Finite(m.bendD)&&m.bendD>=0,
            "material stiffness must be finite and nonnegative");
    Require(Finite(m.dampingRate)&&m.dampingRate>=0&&Finite(m.thickness)&&m.thickness>=0&&Finite(m.airDensity)&&m.airDensity>=0,
            "material damping, thickness, and air density must be finite and nonnegative");
    Require(Finite(m.normalDrag)&&m.normalDrag>=0&&Finite(m.tangentDrag)&&m.tangentDrag>=0&&Finite(m.windAccelerationClamp)&&m.windAccelerationClamp>=0&&Finite(m.contactFriction)&&m.contactFriction>=0,
            "drag, wind cap, and friction must be finite and nonnegative");
    Require(Finite(config.gravity),"gravity must be finite");
    Require(config.artWind.waves.size()<=4,"at most four art wind waves are supported");
    for (const ArtWindWave& wave:config.artWind.waves) {
        Require(Finite(wave.velocityDirection)&&Finite(wave.materialDirection)&&Finite(wave.amplitude)&&Finite(wave.frequencyHz)&&Finite(wave.wavelength)&&Finite(wave.phaseSeed),"art wind values must be finite");
        Require(wave.amplitude>=0&&wave.frequencyHz>=0,"art wind amplitude and frequency must be nonnegative");
        if (wave.amplitude>0) {
            Require(wave.wavelength>0,"enabled art wind wavelength must be positive");
            Require(LengthSq(wave.velocityDirection)>kEpsilon&&wave.materialDirection.x*wave.materialDirection.x+wave.materialDirection.y*wave.materialDirection.y>kEpsilon,
                    "enabled art wind directions must be nonzero");
        }
    }
}

void ValidateStepInputs(const RibbonState& state, const SimulationConfig& config, const std::vector<Pin>& pins,
                        const std::vector<SoftAttachment>& soft, const std::vector<Collider>& colliders) {
    ValidateSimulationConfig(config);
    const size_t count=state.positions.size();
    Require(state.velocities.size()==count,"state position and velocity counts differ");
    for (size_t i=0;i<count;++i) Require(Finite(state.positions[i])&&Finite(state.velocities[i]),"state contains nonfinite values");
    Require(state.windResponse.empty()||state.windResponse.size()==count,"wind response must be empty or per vertex");
    Require(state.permeability.empty()||state.permeability.size()==count,"permeability must be empty or per vertex");
    for (double value:state.windResponse) Require(Finite(value)&&value>=0&&value<=1,"wind response must be finite in [0,1]");
    for (double value:state.permeability) Require(Finite(value)&&value>=0&&value<=1,"permeability must be finite in [0,1]");
    std::unordered_set<uint32_t> hard,attachments;
    for (const Pin& pin:pins) {
        Require(pin.vertex<count&&Finite(pin.targetBegin)&&Finite(pin.targetEnd),"pin index and targets must be valid");
        Require(hard.insert(pin.vertex).second,"duplicate hard pin");
    }
    for (const SoftAttachment& attachment:soft) {
        Require(attachment.vertex<count&&Finite(attachment.target)&&Finite(attachment.compliance)&&Finite(attachment.influence),"soft attachment must be finite and indexed");
        Require(attachments.insert(attachment.vertex).second,"duplicate soft attachment");
        Require(!hard.count(attachment.vertex),"hard and soft attachment overlap");
        if (attachment.enabled) Require(attachment.compliance>0&&attachment.influence>=0&&attachment.influence<=1,"enabled soft attachment needs positive compliance and influence in [0,1]");
    }
    for (const Collider& collider:colliders) {
        switch (collider.type) {
        case Collider::Type::Plane: Require(Finite(collider.plane.point)&&Finite(collider.plane.normal)&&Finite(collider.plane.linearVelocity)&&Finite(collider.plane.angularVelocity)&&LengthSq(collider.plane.normal)>kEpsilon,"plane collider must be finite with nonzero normal"); break;
        case Collider::Type::Sphere: Require(Finite(collider.sphere.center)&&Finite(collider.sphere.linearVelocity)&&Finite(collider.sphere.angularVelocity)&&Finite(collider.sphere.radius)&&collider.sphere.radius>=0,"sphere collider must be finite with nonnegative radius"); break;
        case Collider::Type::Capsule: Require(Finite(collider.capsule.a)&&Finite(collider.capsule.b)&&Finite(collider.capsule.linearVelocity)&&Finite(collider.capsule.angularVelocity)&&Finite(collider.capsule.radius)&&collider.capsule.radius>=0,"capsule collider must be finite with nonnegative radius"); break;
        default: Invalid("unknown collider type");
        }
    }
}

RibbonState::RibbonState(const CookedMesh& mesh)
    : positions(mesh.restPositions), velocities(mesh.restPositions.size()), mesh_(&mesh),
      deltaV_(mesh.restPositions.size()), persistentPins_(mesh.restPositions.size(),false) {
    windResponse.assign(mesh.restPositions.size(),1); permeability.assign(mesh.restPositions.size(),0);
}
void RibbonState::Reset() {
    positions=mesh_->restPositions;
    for (Vec3& position:positions) position-=originDelta_;
    std::fill(velocities.begin(),velocities.end(),Vec3{});
    std::fill(deltaV_.begin(),deltaV_.end(),Vec3{}); std::fill(persistentPins_.begin(),persistentPins_.end(),false);
    accumulator_=0; diagnostics={};
}
void RibbonState::AddExternalDeltaV(uint32_t vertex, Vec3 deltaV) {
    if (vertex>=deltaV_.size() || !Finite(deltaV)) Invalid("external delta-v must be finite and indexed");
    deltaV_[vertex]+=deltaV;
}
void RibbonState::SetPinned(uint32_t vertex, bool pinned) {
    if (vertex>=persistentPins_.size()) Invalid("persistent pin index is invalid");
    persistentPins_[vertex]=pinned;
    if (pinned) velocities[vertex]={};
}
void RibbonState::RebaseOrigin(Vec3 newOriginMinusOldOrigin) {
    if (!Finite(newOriginMinusOldOrigin)) Invalid("origin rebase must be finite");
    originDelta_+=newOriginMinusOldOrigin;
    for (Vec3& p:positions) p-=newOriginMinusOldOrigin;
}

void RibbonState::StepFixed(const SimulationConfig& config, double time, const WindSample& wind,
                            const std::vector<Pin>& pins, const std::vector<SoftAttachment>& soft,
                            const std::vector<Collider>& colliders) {
    Require(Finite(time),"simulation time must be finite");
    Require(positions.size()==mesh_->restPositions.size(),"state topology size changed");
    ValidateStepInputs(*this,config,pins,soft,colliders);
    const double h=config.fixedDt/config.substeps;
    std::vector<double> inverseMass=mesh_->inverseMass;
    for (size_t i=0;i<inverseMass.size();++i) if (persistentPins_[i]) inverseMass[i]=0;
    for (const Pin& pin:pins) if (pin.enabled && pin.vertex<inverseMass.size()) inverseMass[pin.vertex]=0;
    const std::vector<Vec3> tickDeltaV=deltaV_;
    std::fill(deltaV_.begin(),deltaV_.end(),Vec3{});

    for (uint32_t substep=0;substep<config.substeps;++substep) {
        const double substepTime=time+(substep+1)*h;
        std::vector<Vec3> force(positions.size()), aerodynamicForce(positions.size());
        for (size_t i=0;i<force.size();++i) force[i]=config.gravity*mesh_->mass[i];
        for (const TriangleRest& triangle:mesh_->triangles) {
            TriangleIndex t=triangle.indices;
            Vec3 edge1=positions[t.i1]-positions[t.i0],edge2=positions[t.i2]-positions[t.i0];
            Vec3 areaCross=Cross(edge1,edge2); double doubleArea=Length(areaCross), currentArea=0.5*doubleArea;
            if (doubleArea<=std::max(1.e-12,2.e-6*triangle.area)) { ++diagnostics.skippedWindFaces; continue; }
            Vec3 centroid=(positions[t.i0]+positions[t.i1]+positions[t.i2])/3.0;
            Vec3 faceVelocity=(velocities[t.i0]+velocities[t.i1]+velocities[t.i2])/3.0;
            Vec3 airVelocity=wind ? wind(centroid,substepTime) : Vec3{};
            if (!Finite(airVelocity)) Invalid("wind callback returned a nonfinite velocity");
            Vec2 uv={(mesh_->materialCoordinates[t.i0].x+mesh_->materialCoordinates[t.i1].x+mesh_->materialCoordinates[t.i2].x)/3.0,
                     (mesh_->materialCoordinates[t.i0].y+mesh_->materialCoordinates[t.i1].y+mesh_->materialCoordinates[t.i2].y)/3.0};
            for (size_t wi=0;wi<config.artWind.waves.size();++wi) {
                const ArtWindWave& wave=config.artWind.waves[wi];
                double kLength=std::sqrt(wave.materialDirection.x*wave.materialDirection.x+wave.materialDirection.y*wave.materialDirection.y);
                if (wave.amplitude==0 || wave.wavelength<=kEpsilon || kLength<=kEpsilon) continue;
                Vec2 k={wave.materialDirection.x/kLength,wave.materialDirection.y/kLength};
                double phase=2*kPi*((uv.x*k.x+uv.y*k.y)/wave.wavelength-wave.frequencyHz*substepTime)+wave.phaseSeed;
                airVelocity += Normalize(wave.velocityDirection,{})*(wave.amplitude*std::sin(phase));
            }
            Vec3 normal=areaCross/(2*currentArea),relativeAir=airVelocity-faceVelocity;
            double normalSpeed=Dot(relativeAir,normal); Vec3 tangentAir=relativeAir-normal*normalSpeed;
            Vec3 load=normal*(0.5*config.material.airDensity*config.material.normalDrag*currentArea*normalSpeed*std::abs(normalSpeed));
            load+=tangentAir*(0.5*config.material.airDensity*config.material.tangentDrag*currentArea*Length(tangentAir));
            double response=Clamp(FaceAverage(windResponse,t,1),0,1)*(1-Clamp(FaceAverage(permeability,t,0),0,1));
            load*=response;
            aerodynamicForce[t.i0]+=load/3.0; aerodynamicForce[t.i1]+=load/3.0; aerodynamicForce[t.i2]+=load/3.0;
        }
        // Clamp total aerodynamic acceleration per vertex before gravity. This does
        // not scale with face valence and leaves gravity physically unaffected.
        for (size_t i=0;i<force.size();++i) {
            Vec3 aero=aerodynamicForce[i];
            if (config.material.windAccelerationClamp>0) {
                double maxForce=config.material.windAccelerationClamp*mesh_->mass[i];
                if (Length(aero)>maxForce) { aero*=maxForce/Length(aero); ++diagnostics.windClamps; }
            }
            force[i]+=aero;
        }

        std::vector<Vec3> old=positions,freeVelocity(positions.size());
        double damping=std::exp(-config.material.dampingRate*h);
        for (size_t i=0;i<positions.size();++i) {
            if (inverseMass[i]==0) { freeVelocity[i]=velocities[i]; continue; }
            velocities[i]=(velocities[i]+force[i]*(inverseMass[i]*h)+(substep==0 ? tickDeltaV[i]:Vec3{}))*damping;
            freeVelocity[i]=velocities[i]; positions[i]+=velocities[i]*h;
        }
        for (const Pin& pin:pins) if (pin.enabled && pin.vertex<positions.size()) {
            double a=double(substep+1)/config.substeps;
            Vec3 target=pin.targetBegin*(1-a)+pin.targetEnd*a;
            positions[pin.vertex]=target; velocities[pin.vertex]=(target-old[pin.vertex])/h; freeVelocity[pin.vertex]=velocities[pin.vertex];
        }

        std::vector<double> lambdaU(mesh_->triangles.size()),lambdaV(lambdaU),lambdaS(lambdaU),lambdaB(mesh_->hinges.size()),lambdaA(soft.size());
        for (uint32_t iteration=0;iteration<config.iterations;++iteration) {
            for (uint32_t color=0;color<mesh_->triangleColorCount;++color) for (size_t ti=0;ti<mesh_->triangles.size();++ti) {
                if (mesh_->triangleColors[ti]!=color) continue;
                const TriangleRest& triangle=mesh_->triangles[ti]; TriangleIndex t=triangle.indices;
                std::array<uint32_t,3> ids={t.i0,t.i1,t.i2};
                TriangleEvaluation e=EvaluateTriangle(triangle,positions);
                if (!e.valid) { ++diagnostics.skippedTriangleConstraints; continue; }
                if (config.material.ku>0) ProjectScalar(positions,ids,e.gradU,inverseMass,e.cu,1/(triangle.area*config.material.ku*h*h),lambdaU[ti]);
                e=EvaluateTriangle(triangle,positions);
                if (e.valid && config.material.kv>0) ProjectScalar(positions,ids,e.gradV,inverseMass,e.cv,1/(triangle.area*config.material.kv*h*h),lambdaV[ti]);
                e=EvaluateTriangle(triangle,positions);
                if (e.valid && config.material.ks>0) ProjectScalar(positions,ids,e.gradS,inverseMass,e.cs,1/(triangle.area*config.material.ks*h*h),lambdaS[ti]);
            }
            for (size_t ai=0;ai<soft.size();++ai) {
                const SoftAttachment& attachment=soft[ai];
                if (!attachment.enabled || attachment.vertex>=positions.size() || attachment.compliance<=0 || attachment.influence<=0 || inverseMass[attachment.vertex]==0) continue;
                Vec3 displacement=positions[attachment.vertex]-attachment.target; double distance=Length(displacement);
                if (distance<=kEpsilon) continue;
                double alphaTilde=attachment.compliance/(attachment.influence*h*h);
                double denominator=inverseMass[attachment.vertex]+alphaTilde;
                double deltaLambda=(-distance-alphaTilde*lambdaA[ai])/denominator;
                lambdaA[ai]+=deltaLambda;
                positions[attachment.vertex]+=Normalize(displacement)*(inverseMass[attachment.vertex]*deltaLambda);
            }
            if (config.material.bendD>0) for (uint32_t color=0;color<mesh_->hingeColorCount;++color) for (size_t hi=0;hi<mesh_->hinges.size();++hi) {
                if (mesh_->hingeColors[hi]!=color) continue;
                const HingeRest& hinge=mesh_->hinges[hi]; HingeEvaluation e=EvaluateHinge(hinge,positions);
                if (!e.valid) { ++diagnostics.skippedHinges; continue; }
                double stiffness=config.material.bendD*hinge.edgeLength/hinge.dualWidth;
                ProjectHinge(positions,hinge,e,inverseMass,1/(stiffness*h*h),lambdaB[hi]);
            }
            for (size_t i=0;i<positions.size();++i) for (const Collider& collider:colliders)
                ProjectContact(positions[i],inverseMass[i],config.material.thickness,collider);
        }
        for (size_t i=0;i<positions.size();++i) if (inverseMass[i]>0) {
            velocities[i]=(positions[i]-old[i])/h;
            for (const Collider& collider:colliders)
                ResolveContactVelocity(positions[i],velocities[i],freeVelocity[i],config.material.thickness,config.material.contactFriction,collider);
        }
    }
    ++diagnostics.fixedTicks; diagnostics.accumulator=accumulator_;
}

void RibbonState::Advance(double realDt, const SimulationConfig& config, double startTime, const WindSample& wind,
                          const std::vector<Pin>& pins, const std::vector<SoftAttachment>& soft,
                          const std::vector<Collider>& colliders) {
    ValidateStepInputs(*this,config,pins,soft,colliders);
    Require(Finite(realDt)&&Finite(startTime),"advance time values must be finite");
    if (realDt<=0) return;
    double firstTickStart=startTime-accumulator_;
    accumulator_+=realDt; uint32_t ticks=0;
    while (accumulator_+1e-14>=config.fixedDt && ticks<config.maxTicksPerAdvance) {
        StepFixed(config,firstTickStart+ticks*config.fixedDt,wind,pins,soft,colliders);
        accumulator_-=config.fixedDt; ++ticks;
    }
    if (accumulator_>=config.fixedDt) {
        uint64_t dropped=uint64_t(accumulator_/config.fixedDt);
        diagnostics.droppedTicks+=dropped; accumulator_-=dropped*config.fixedDt;
    }
    diagnostics.accumulator=accumulator_;
}
} // namespace taribbon
