/* Hand-edited, against evidence. G3i has no instance state: both of the
 * class's functions write the geometry engine's matrix ports through literal
 * hardware addresses (0x4000440 selects the matrix mode, 0x4000458/0x400045c
 * stream the projection/position rows) and divide through the 64-bit math
 * coprocessor at 0x4000290. Neither takes a `this` -- every parameter comes
 * from the mangled name, the same shape as include/G2x.h and include/OAM.h.
 *
 * PerspectiveW_ is NOT declared here. Its mangled name
 * (_ZN3G3i13PerspectiveW_E5Fix12IiES1_S1_S1_S1_S1_bP9Matrix4x3) carries six
 * by-value Fix12<int> parameters, the by-value-class ABI wall of
 * include/OAM.h's note: callers keep `extern "C"` declarations of the literal
 * symbol, and the definition in engine/gx/G3i.cpp stays literal-mangled under
 * `#pragma cplusplus off` because the ROM body is the C front end's output.
 */
#ifndef G3I_H
#define G3I_H
#include "types.h"

struct Matrix4x3;

struct G3i {
#ifdef __cplusplus
    /* All static -- see the header note. */
    static void LookAt_(const Vector3 *at, const Vector3 *up,
                        const Vector3 *eye, bool draw, Matrix4x3 *mat);
#endif
};

#endif
