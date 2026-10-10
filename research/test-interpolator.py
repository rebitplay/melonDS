"""Compile baseline/candidate interpolators extracted verbatim, compare in WASM."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research/interpolator"
OUT.mkdir(parents=True, exist_ok=True)
baseline = subprocess.check_output(["git", "show", "HEAD:src/GPU3D_Soft.h"], cwd=ROOT, text=True)
candidate = (ROOT / "src/GPU3D_Soft.h").read_text()


def extract(text):
    start = text.index("    template<int dir>\n    class Interpolator")
    end = text.index("    template<int side>", start)
    return text[start:end]


source = """
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <wasm_simd128.h>
using s32=int32_t; using u32=uint32_t; using s64=int64_t;
#define REBIT_MELONDS_LATE_PERSPECTIVE 1
namespace reference {
""" + extract(baseline) + "\n}\nnamespace candidate {\n" + extract(candidate) + """
}
u32 seed=1234567;
u32 random32() { return seed=seed*1664525U+1013904223U; }
template<int dir> u32 check() {
    u32 checks=0;
    for(u32 trial=0;trial<100000;++trial) {
        const s32 start=s32(random32()%1024)-512, span=trial%19==0 ? 0 : random32()%512+1;
        s32 w0=random32()%65536,w1=random32()%65536;
        if(trial%4==0) w0=w1=0;
        if(trial%4==1) w0=w1=w0&~127;
        if(trial%4==2) w0=w1;
        const bool wb=((trial/4)%2)==0;
        reference::Interpolator<dir> ref(start,start+span,w0,w1,wb);
        candidate::Interpolator<dir> lazy(start,start+span,w0,w1,wb), eager(start,start+span,w0,w1,wb);
        for(u32 pixel=0;pixel<8;++pixel) {
            const s32 x=start+(span ? random32()%(span+1):0);
            const s32 z0=random32()%0x1000000, z1=random32()%0x1000000;
            ref.SetX(x);lazy.SetXForDepth(x);eager.SetX(x);
            assert(ref.InterpolateZ(z0,z1)==lazy.InterpolateZ(z0,z1));
            assert(ref.InterpolateZ(z0,z1)==eager.InterpolateZ(z0,z1));
            checks+=2;
            // A depth-rejected pixel never prepares attributes. The next pixel
            // must not accidentally reuse its previous perspective factor.
            if(pixel%3==0) continue;
            lazy.PrepareAttributes();
            for(u32 attr=0;attr<5;++attr) {
                const s32 a=s32(random32()%65536)-32768,b=attr==0?a:s32(random32()%65536)-32768;
                assert(ref.Interpolate(a,b)==lazy.Interpolate(a,b));
                assert(ref.Interpolate(a,b)==eager.Interpolate(a,b));
                const v128_t va=wasm_i32x4_make(a,b,-a,0), vb=wasm_i32x4_make(b,a,-b,0);
                const v128_t result=lazy.Interpolate4(va,vb);
                assert(wasm_i32x4_extract_lane(result,0)==ref.Interpolate(a,b));
                assert(wasm_i32x4_extract_lane(result,1)==ref.Interpolate(b,a));
                assert(wasm_i32x4_extract_lane(result,2)==ref.Interpolate(-a,-b));
                assert(wasm_i32x4_extract_lane(result,3)==0);
                checks+=6;
            }
        }
    }
    return checks;
}
int main() { std::printf("PASS: %u exact interpolation comparisons (X/Y, Z/W, zero spans/denominators, linear/perspective, skipped pixels)\\n",check<0>()+check<1>()); }
"""
(OUT / "test.cpp").write_text(source)
compiler = "/home/daudau/Code/emsdk/upstream/emscripten/em++"
subprocess.run([compiler, "-std=c++17", "-O3", "-msimd128", "-fwrapv", "-sENVIRONMENT=node", "-sSINGLE_FILE=1",
                str(OUT / "test.cpp"), "-o", str(OUT / "test.cjs")], check=True)
subprocess.run(["node", str(OUT / "test.cjs")], check=True)
