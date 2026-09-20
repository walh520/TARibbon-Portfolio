#include "RibbonRenderBinding.h"
#include <cmath>
#include <stdexcept>

namespace taribbon {
static Vec3 TangentFallback(Vec3 n) {
    return Normalize(Cross(std::abs(n.z)<0.8 ? Vec3{0,0,1}:Vec3{0,1,0},n),{1,0,0});
}
bool BindToTriangle(const CookedMesh& mesh,uint32_t tid,Vec3 p,RenderBinding& out,double tol) {
    if(tid>=mesh.triangles.size() || !std::isfinite(tol) || tol<0) return false;
    const auto& t=mesh.triangles[tid]; const auto i=t.indices;
    Vec3 a=mesh.restPositions[i.i0],e=mesh.restPositions[i.i1]-a,f=mesh.restPositions[i.i2]-a;
    Vec3 n=Normalize(Cross(e,f),t.restNormal); double off=Dot(p-a,n); Vec3 q=p-a-n*off;
    double ee=Dot(e,e),ef=Dot(e,f),ff=Dot(f,f),qe=Dot(q,e),qf=Dot(q,f),det=ee*ff-ef*ef;
    if(!(det>1e-24) || !std::isfinite(det)) return false;
    double b1=(qe*ff-qf*ef)/det,b2=(qf*ee-qe*ef)/det,b0=1-b1-b2;
    if(!std::isfinite(b0+b1+b2+off) || b0<-tol || b1<-tol || b2<-tol ||
        b0>1+tol || b1>1+tol || b2>1+tol) return false;
    out={tid,{b0,b1,b2},off,1}; return true;
}
std::vector<SurfaceFrame> BuildSimulationFrames(const CookedMesh& m,const std::vector<Vec3>& x) {
    if(x.size()!=m.restPositions.size()) throw std::invalid_argument("render state vertex count mismatch");
    std::vector<SurfaceFrame> frames(x.size());
    std::vector<Vec3> normals(x.size()),tangents(x.size()),restNormals(x.size());
    for(const auto& t:m.triangles) {
        auto i=t.indices; Vec3 a=x[i.i1]-x[i.i0],b=x[i.i2]-x[i.i0];
        Vec3 n=Cross(a,b); auto ev=EvaluateTriangle(t,x);
        Vec3 u=ev.valid ? ev.Fu:t.fiberU;
        for(uint32_t v:{i.i0,i.i1,i.i2}) { normals[v]+=n; tangents[v]+=u*t.area; restNormals[v]+=t.restNormal*t.area; }
    }
    for(size_t v=0;v<x.size();++v) {
        Vec3 n=Normalize(normals[v],Normalize(restNormals[v]));
        Vec3 t=Normalize(tangents[v]-n*Dot(n,tangents[v]),TangentFallback(n));
        frames[v]={x[v],n,t,1};
    }
    return frames;
}
SurfaceFrame EvaluateRenderBinding(const CookedMesh& m,const RenderBinding& b,
    const std::vector<Vec3>& x,const std::vector<SurfaceFrame>& frames) {
    if(b.triangle>=m.triangles.size() || x.size()!=m.restPositions.size() || frames.size()!=x.size())
        throw std::invalid_argument("invalid render binding buffers");
    const auto& t=m.triangles[b.triangle]; const auto i=t.indices;
    Vec3 w=b.barycentric;
    Vec3 n=Normalize(frames[i.i0].normal*w.x+frames[i.i1].normal*w.y+frames[i.i2].normal*w.z,t.restNormal);
    Vec3 tangent=frames[i.i0].tangent*w.x+frames[i.i1].tangent*w.y+frames[i.i2].tangent*w.z;
    tangent=Normalize(tangent-n*Dot(n,tangent),TangentFallback(n));
    // Offset uses deformed FACE normal so binding reconstructs the exact rest point;
    // shading normal remains smoothly interpolated independently.
    Vec3 faceNormal=Normalize(Cross(x[i.i1]-x[i.i0],x[i.i2]-x[i.i0]),t.restNormal);
    return {x[i.i0]*w.x+x[i.i1]*w.y+x[i.i2]*w.z+faceNormal*b.normalOffset,n,tangent,b.tangentSign};
}
}
