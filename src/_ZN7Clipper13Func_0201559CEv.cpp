//cpp
// @symbol _ZN7Clipper13Func_0201559CEv
/* Clipper::Func_0201559C -- rebuild the four view-frustum side planes from the
 * current field of view and near-plane distance.
 *
 * mFovAngle is an angle; its top 12 bits index a sine/cosine pair table at
 * data_02082214, and fdiv of that pair gives the tangent. Scaling by mNearZ
 * (the plane distance) gives the half-height halfHeight, and by mAspectRatio (the aspect
 * ratio) the half-width halfWidth. The four corner vectors at z = -mNearZ follow, and
 * each adjacent pair crossed and normalised is one side plane's normal.
 *
 * This body is what proves mAspectRatio is SIGNED: it sign-extends the field into a
 * 64-bit multiply, which a u32 cannot do. The header used to type it u32 on the
 * weaker evidence of Func_020156DC, which only stores to it; that is now
 * corrected there rather than cast away here.
 *
 * The name is still the placeholder; renaming it is a symbols.txt change.
 */
#include "Clipper.h"

extern "C" int _ZN4cstd4fdivEii(int a, int b);
extern "C" void CrossVec3(Vector3 *a, Vector3 *b, Vector3 *result);
extern "C" void NormalizeVec3(Vector3 *src, Vector3 *dst);
extern "C" short data_02082214[];

void Clipper::Func_0201559C()
{
    int angleIndex = (int)mFovAngle >> 4;
    int fovTangent = _ZN4cstd4fdivEii(data_02082214[2 * angleIndex], data_02082214[2 * angleIndex + 1]);
    Fix12i halfHeight = (Fix12i)(((long long)mNearZ * fovTangent + 0x800) >> 12);
    Fix12i halfWidth = (Fix12i)(((long long)mAspectRatio * halfHeight + 0x800) >> 12);
    Vector3 bottomLeft, topLeft, topRight, bottomRight;
    bottomLeft.x = -halfWidth;
    bottomLeft.y = -halfHeight;
    bottomLeft.z = -mNearZ;
    topLeft.x = -halfWidth;
    topLeft.y = halfHeight;
    topLeft.z = -mNearZ;
    topRight.x = halfWidth;
    topRight.y = halfHeight;
    topRight.z = -mNearZ;
    bottomRight.x = halfWidth;
    bottomRight.y = -halfHeight;
    bottomRight.z = -mNearZ;
    CrossVec3(&topLeft, &bottomLeft, (Vector3 *)&mPlaneNormals[0]);
    CrossVec3(&topRight, &topLeft, (Vector3 *)&mPlaneNormals[1]);
    CrossVec3(&bottomRight, &topRight, (Vector3 *)&mPlaneNormals[2]);
    CrossVec3(&bottomLeft, &bottomRight, (Vector3 *)&mPlaneNormals[3]);
    NormalizeVec3((Vector3 *)&mPlaneNormals[0], (Vector3 *)&mPlaneNormals[0]);
    NormalizeVec3((Vector3 *)&mPlaneNormals[1], (Vector3 *)&mPlaneNormals[1]);
    NormalizeVec3((Vector3 *)&mPlaneNormals[2], (Vector3 *)&mPlaneNormals[2]);
    NormalizeVec3((Vector3 *)&mPlaneNormals[3], (Vector3 *)&mPlaneNormals[3]);
}
