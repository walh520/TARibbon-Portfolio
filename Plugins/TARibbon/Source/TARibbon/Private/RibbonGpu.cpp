#include "RibbonGpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace taribbon::gpu {
float UIntBits(uint32_t n) { float f; std::memcpy(&f,&n,4); return f; }
uint32_t ReadUIntBits(float f) { uint32_t n; std::memcpy(&n,&f,4); return n; }
static float F(double x) {
    if(!std::isfinite(x) || std::abs(x)>std::numeric_limits<float>::max())
        throw std::invalid_argument("nonfinite/out-of-range GPU input");
    return static_cast<float>(x);
}
static Float4 V(Vec3 v,double w=0) { return {F(v.x),F(v.y),F(v.z),F(w)}; }
static uint32_t Count(size_t n) {
    if(n>std::numeric_limits<uint32_t>::max()) throw std::length_error("GPU index overflow");
    return static_cast<uint32_t>(n);
}
std::vector<UInt4> PackUInts(const std::vector<uint32_t>& a) {
    std::vector<UInt4> packed(std::max<size_t>(1,(a.size()+3)/4));
    for(size_t i=0;i<a.size();++i) {
        auto& q=packed[i/4];
        switch(i%4) {case 0:q.x=a[i];break;case 1:q.y=a[i];break;case 2:q.z=a[i];break;default:q.w=a[i];}
    }
    return packed;
}
static void ColorBuffers(Upload& out,const std::string& prefix,const std::vector<uint32_t>& colors,uint32_t nc) {
    std::vector<uint32_t> refs; std::vector<UInt4> ranges;
    for(uint32_t c=0;c<nc;++c) {
        uint32_t first=Count(refs.size());
        for(uint32_t i=0;i<colors.size();++i) if(colors[i]==c) refs.push_back(i);
        ranges.push_back({first,Count(refs.size())-first,0,0});
    }
    if(ranges.empty()) ranges.resize(1);
    out.uints["Packed"+prefix+"ColorRefs"]=PackUInts(refs);
    out.uints[prefix+"ColorRanges"]=std::move(ranges);
}
void UpdateColliderInputs(Upload& out,const std::vector<Collider>& cs,double friction) {
    if(!std::isfinite(friction)||friction<0) throw std::invalid_argument("invalid friction");
    const size_t n=std::max<size_t>(1,cs.size());
    for(auto key:{"ColliderMeta","ColliderGeometry0","ColliderGeometry1","ColliderMotion0","ColliderMotion1"})
        out.floats[key].assign(n,{});
    for(size_t i=0;i<cs.size();++i) {
        const auto& c=cs[i]; uint32_t type=0; Float4 g0,g1,v,omega;
        if(c.type==Collider::Type::Plane) {
            if(LengthSq(c.plane.normal)<1e-20) throw std::invalid_argument("plane normal must be nonzero");
            Vec3 nrm=Normalize(c.plane.normal);
            g0=V(nrm,Dot(nrm,c.plane.point)); g1=V(c.plane.point);
            v=V(c.plane.linearVelocity); omega=V(c.plane.angularVelocity);
        } else if(c.type==Collider::Type::Sphere) {
            type=1; if(c.sphere.radius<0) throw std::invalid_argument("negative sphere radius");
            g0=V(c.sphere.center,c.sphere.radius); g1=V(c.sphere.center);
            v=V(c.sphere.linearVelocity); omega=V(c.sphere.angularVelocity);
        } else if(c.type==Collider::Type::Capsule) {
            type=2; if(c.capsule.radius<0) throw std::invalid_argument("negative capsule radius");
            g0=V(c.capsule.a,c.capsule.radius); g1=V(c.capsule.b);
            v=V(c.capsule.linearVelocity); omega=V(c.capsule.angularVelocity);
        } else throw std::invalid_argument("invalid collider type");
        out.floats["ColliderMeta"][i]={UIntBits(type),F(friction),0,0};
        out.floats["ColliderGeometry0"][i]=g0; out.floats["ColliderGeometry1"][i]=g1;
        out.floats["ColliderMotion0"][i]=v; out.floats["ColliderMotion1"][i]=omega;
    }
    out.parameters.Params3.w=UIntBits(Count(cs.size()));
}
void UpdatePinTargetInputs(Upload& out,const std::vector<Pin>& pins,const std::vector<SoftAttachment>& soft,
    double a0,double a1) {
    if(!(a0>=0 && a0<=a1 && a1<=1)) throw std::invalid_argument("invalid pin fractions");
    const uint32_t n=ReadUIntBits(out.parameters.Params3.x); std::set<uint32_t> used;
    for(auto& v:out.floats.at("VertexRest")) v.w=0; // removed soft pins must actually release
    for(const auto& p:pins) if(p.enabled) {
        if(p.vertex>=n || !used.insert(p.vertex).second) throw std::invalid_argument("duplicate/out-of-range pin");
        out.floats.at("PinTarget")[p.vertex]=V(p.targetBegin*(1-a1)+p.targetEnd*a1);
        out.floats.at("PinPreviousTarget")[p.vertex]=V(p.targetBegin*(1-a0)+p.targetEnd*a0);
    }
    for(const auto& s:soft) if(s.enabled) {
        if(s.vertex>=n || !used.insert(s.vertex).second || s.compliance<=0 || s.influence<0 || s.influence>1)
            throw std::invalid_argument("invalid/duplicate soft pin");
        out.floats.at("PinTarget")[s.vertex]=V(s.target,s.compliance);
        out.floats.at("VertexRest")[s.vertex].w=F(s.influence);
    }
}
Upload BuildInitialUpload(const CookedMesh& m,const SimulationConfig& cfg,const std::vector<RenderBinding>& bindings,
    const std::vector<Pin>& pins,const std::vector<SoftAttachment>& soft,const std::vector<Collider>& cs) {
    ValidateSimulationConfig(cfg);
    if(m.restPositions.empty()||m.triangles.empty() || cfg.substeps==0 || cfg.iterations==0 || cfg.fixedDt<=0)
        throw std::invalid_argument("empty mesh or invalid simulation step");
    if(cfg.artWind.waves.size()>4) throw std::invalid_argument("GPU supports at most four coherent art waves");
    Upload o; const auto& mat=cfg.material; const uint32_t n=Count(m.restPositions.size()),nt=Count(m.triangles.size()),nh=Count(m.hinges.size());
    double h=cfg.fixedDt/cfg.substeps;
    o.parameters.Params0={F(h),F(1/h),F(mat.dampingRate),1};
    o.parameters.Params1={F(mat.airDensity),0,F(mat.thickness),1e-7f};
    o.parameters.Params2={1,0,0,F(mat.windAccelerationClamp)};
    o.parameters.Params3={UIntBits(n),UIntBits(nt),UIntBits(nh),UIntBits(Count(cs.size()))};
    o.parameters.GravityAndTime=V(cfg.gravity);
    o.parameters.ArtWindParams={cfg.artWind.waves.empty()?0.f:1.f,F(cfg.artWind.waves.size()),1,0};
    o.parameters.PassParams={0,UIntBits(Count(bindings.size())),0,0};
    for(auto name:{"State0","State1","VertexRest","PinTarget","PinPreviousTarget","ExternalImpulse","SubstepStartPos",
                  "FreeVelocity","SoftPinLambda","VertexNormal","VertexTangent","VertexRestNormal"}) o.floats[name].resize(n);
    auto frames=BuildSimulationFrames(m,m.restPositions);
    for(uint32_t i=0;i<n;++i) {
        o.floats["State0"][i]=V(m.restPositions[i],m.inverseMass[i]);
        o.floats["VertexRest"][i]=V(m.restPositions[i]);
        o.floats["PinTarget"][i]=o.floats["PinPreviousTarget"][i]=V(m.restPositions[i]);
        o.floats["SubstepStartPos"][i]=V(m.restPositions[i]);
        o.floats["VertexNormal"][i]=o.floats["VertexRestNormal"][i]=V(frames[i].normal);
        o.floats["VertexTangent"][i]=V(frames[i].tangent);
    }
    for(const auto& p:pins) if(p.enabled) {
        if(p.vertex>=n || Length(p.targetBegin-m.restPositions[p.vertex])>1e-7 || Length(p.targetEnd-p.targetBegin)>1e-7)
            throw std::invalid_argument("initial hard pin must coincide with rest vertex; move it on later substeps");
        o.floats["State0"][p.vertex].w=0; o.floats["State1"][p.vertex].w=UIntBits(1);
    }
    UpdatePinTargetInputs(o,pins,soft,0,0);
    std::vector<std::vector<uint32_t>> adjacency(n);
    for(auto name:{"TriRest0","TriRest1","TriRest2","TriRest3","TriRest4","TriArtCoord","FaceWind","FaceAeroForce","TriangleLambda"}) o.floats[name].resize(nt);
    o.uints["TriIndices"].resize(nt);
    for(uint32_t j=0;j<nt;++j) {
        const auto& t=m.triangles[j]; const auto i=t.indices; const auto& r=t.invDm;
        o.uints["TriIndices"][j]={i.i0,i.i1,i.i2,3};
        o.floats["TriRest0"][j]=V(t.fiberU,t.area);
        o.floats["TriRest1"][j]=V(Cross(t.restNormal,t.fiberU),r[0]);
        o.floats["TriRest2"][j]={F(r[1]),F(r[2]),F(r[3]),F(mat.ku)};
        o.floats["TriRest3"][j]={F(mat.kv),F(mat.ks),F(mat.normalDrag),F(mat.tangentDrag)};
        o.floats["TriRest4"][j]={1,0,0,0}; // host may upload painted face masks
        Vec2 uv{(m.materialCoordinates[i.i0].x+m.materialCoordinates[i.i1].x+m.materialCoordinates[i.i2].x)/3,
                (m.materialCoordinates[i.i0].y+m.materialCoordinates[i.i1].y+m.materialCoordinates[i.i2].y)/3};
        o.floats["TriArtCoord"][j]={F(uv.x),F(uv.y),0,1};
        for(uint32_t v:{i.i0,i.i1,i.i2}) adjacency[v].push_back(j);
    }
    std::vector<uint32_t> offsets{0},refs;
    for(const auto& a:adjacency) { refs.insert(refs.end(),a.begin(),a.end()); offsets.push_back(Count(refs.size())); }
    o.uints["PackedVertexFaceOffsets"]=PackUInts(offsets); o.uints["PackedVertexFaceRefs"]=PackUInts(refs);
    ColorBuffers(o,"Triangle",m.triangleColors,m.triangleColorCount);
    o.uints["HingeIndices"].resize(std::max(1u,nh));
    o.floats["HingeRest"].resize(std::max(1u,nh)); o.floats["HingeLambda"].resize(std::max(1u,nh));
    for(uint32_t j=0;j<nh;++j) { const auto& hng=m.hinges[j];
        o.uints["HingeIndices"][j]={hng.p0,hng.p1,hng.p2,hng.p3};
        o.floats["HingeRest"][j]={F(hng.restAngle),F(hng.edgeLength),F(hng.dualWidth),F(mat.bendD)};
    }
    ColorBuffers(o,"Hinge",m.hingeColors,m.hingeColorCount);
    for(auto name:{"ArtWave0","ArtWave1","ArtWave2"}) o.floats[name].resize(4);
    for(size_t j=0;j<cfg.artWind.waves.size();++j) { const auto& w=cfg.artWind.waves[j];
        if(w.wavelength<=0) throw std::invalid_argument("nonpositive art wavelength");
        o.floats["ArtWave0"][j]=V(Normalize(w.velocityDirection,{1,0,0}),w.amplitude);
        o.floats["ArtWave1"][j]={F(w.frequencyHz),F(w.wavelength),F(std::remainder(w.phaseSeed,2*kPi)),1};
        double len=std::hypot(w.materialDirection.x,w.materialDirection.y);
        o.floats["ArtWave2"][j]=len>1e-12 ? Float4{F(w.materialDirection.x/len),F(w.materialDirection.y/len),0,0}:Float4{1,0,0,0};
    }
    const uint32_t nr=Count(bindings.size()); o.uints["RenderBinding"].resize(std::max(1u,nr));
    for(auto name:{"RenderBaryOffset","RenderPosition","RenderPreviousPosition","RenderNormal","RenderTangent"}) o.floats[name].resize(std::max(1u,nr));
    for(uint32_t j=0;j<nr;++j) { const auto& b=bindings[j];
        if(b.triangle>=nt || std::abs(b.barycentric.x+b.barycentric.y+b.barycentric.z-1)>1e-6 ||
            b.barycentric.x<0 || b.barycentric.y<0 || b.barycentric.z<0) throw std::invalid_argument("invalid render barycentrics");
        auto i=m.triangles[b.triangle].indices; auto f=EvaluateRenderBinding(m,b,m.restPositions,frames);
        o.uints["RenderBinding"][j]={i.i0,i.i1,i.i2,b.tangentSign<0?1u:0u};
        o.floats["RenderBaryOffset"][j]=V(b.barycentric,b.normalOffset);
        o.floats["RenderPosition"][j]=o.floats["RenderPreviousPosition"][j]=V(f.position);
        o.floats["RenderNormal"][j]=V(f.normal); o.floats["RenderTangent"][j]=V(f.tangent,b.tangentSign);
    }
    o.uints["Diagnostics"].resize(1); UpdateColliderInputs(o,cs,mat.contactFriction);
    return o;
}
static void Add(std::vector<Command>& p,const char* entry,uint32_t count,uint32_t threads,uint32_t sub,
    uint32_t iteration,double time,uint32_t color=0) {
    if(count==0) return;
    p.push_back({CommandKind::Dispatch,entry,(count+threads-1)/threads,sub,iteration,color,time,true});
}
std::vector<Command> BuildTickPlan(const CookedMesh& m,const SimulationConfig& cfg,double start) {
    ValidateSimulationConfig(cfg);
    if(cfg.substeps==0||cfg.iterations==0||!(cfg.fixedDt>0)||!std::isfinite(start)) throw std::invalid_argument("invalid tick plan");
    std::vector<Command> p; Add(p,"ClearDiagnostics",1,1,0,0,start);
    const uint32_t n=Count(m.restPositions.size()),nt=Count(m.triangles.size()),nh=Count(m.hinges.size());
    std::vector<uint32_t> tc(m.triangleColorCount),hc(m.hingeColorCount);
    for(uint32_t c:m.triangleColors) ++tc.at(c);
    for(uint32_t c:m.hingeColors) ++hc.at(c);
    for(uint32_t s=0;s<cfg.substeps;++s) {
        double t=start+(s+1)*cfg.fixedDt/cfg.substeps;
        p.push_back({CommandKind::SampleExternalInputs,"",0,s,0,0,t,true});
        Add(p,"ComputeFaceAeroForces",nt,128,s,0,t); Add(p,"PredictVertices",n,128,s,0,t);
        Add(p,"ResetTriangleLambdas",nt,128,s,0,t); Add(p,"ResetHingeLambdas",nh,128,s,0,t); Add(p,"ResetSoftPinLambdas",n,128,s,0,t);
        for(uint32_t it=0;it<cfg.iterations;++it) {
            for(uint32_t c=0;c<tc.size();++c) Add(p,"SolveTriangleColor",tc[c],64,s,it,t,c);
            Add(p,"SolveSoftPins",n,128,s,it,t);
            for(uint32_t c=0;c<hc.size();++c) Add(p,"SolveHingeColor",hc[c],64,s,it,t,c);
            Add(p,"ProjectContacts",n,128,s,it,t);
        }
        Add(p,"UpdateVelocitiesAndApplyContactFriction",n,128,s,0,t);
    }
    return p;
}
std::vector<Command> BuildRenderPlan(uint32_t n,uint32_t r) {
    std::vector<Command> p; Add(p,"GatherVertexFrames",n,128,0,0,0); Add(p,"BuildRenderVertices",r,128,0,0,0); return p;
}
}
