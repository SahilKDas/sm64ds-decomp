/* HOST COPY of func_ov021_02111434 (ov021 0x02111434, 0x1a8): WorkElevator's
 * per-frame cage-matrix builder -- quaternion orientation into the shared
 * scratch matrix, then the four car matrices at +0x33c (0x50 stride) from
 * the offset table data_ov021_02114a20 and the yaw table data_ov021_02114740.
 *
 * Transcribed line for line from src/actors/daObjCvNewsLift_c.cpp. The ONE change:
 * the matched TU ends in a two-instruction mwcc `asm` veneer
 * (`ands r0, r0, #1; bx lr` -- a laundering lever the byte gate needed), and
 * MSVC has no mwcc asm. The veneer IS `x & 1`; the copy spells it that way.
 * Same class as U5's inline-asm excision ask (census part 1) -- when hostgen
 * grows the excision, the matched TU takes this file's slice line back.
 */
extern "C" {
struct Quaternion;
struct Matrix4x3 { int m[12]; };

void Matrix4x3_FromQuaternion(const struct Quaternion *q, struct Matrix4x3 *mF);
void Vec3_Asr(void *d, void *s, int sh);
void Matrix4x3_FromTranslation(struct Matrix4x3 *m, int x, int y, int z);
void MulMat4x3Mat4x3(void *a, void *b, void *c);
void Matrix4x3_ApplyInPlaceToRotationX(struct Matrix4x3 *mF, short angX);
void Matrix4x3_ApplyInPlaceToRotationZ(struct Matrix4x3 *mF, short angZ);
void Matrix4x3_ApplyInPlaceToRotationY(struct Matrix4x3 *mF, short angY);
void Matrix4x3_ApplyInPlaceToTranslation(struct Matrix4x3 *mF, int x, int y,
                                         int z);

extern struct Matrix4x3 data_020a0e68;
extern const int data_ov021_02114a20[];
extern const short data_ov021_02114740[];
}

/* PORT_HOST_ABI: the matched TU ends in a two-instruction mwcc asm veneer
   (ands r0,r0,#1; bx lr) MSVC cannot compile; the copy spells it & 1. */
extern "C" int func_ov021_02111434(char *c)
{
    struct Matrix4x3 mtx;
    int i;
    char *src;
    char *tr;
    char *obj;
    unsigned rv;
    volatile int v[3];
    int tmp[3];
    int vo[3];

    Matrix4x3_FromQuaternion((struct Quaternion *)(c + 0xc4c), &mtx);
    Vec3_Asr(tmp, c + 0x5c, 3);
    Matrix4x3_FromTranslation(&data_020a0e68, tmp[0], tmp[1], tmp[2]);
    MulMat4x3Mat4x3(&mtx, &data_020a0e68, &data_020a0e68);
    Matrix4x3_ApplyInPlaceToRotationX(&data_020a0e68, *(short *)(c + 0x8c));
    Matrix4x3_ApplyInPlaceToRotationZ(&data_020a0e68, *(short *)(c + 0x90));
    rv = 1;
    Matrix4x3_ApplyInPlaceToRotationY(&data_020a0e68, *(short *)(c + 0x8e));
    *(struct Matrix4x3 *)(c + 0xf0) = data_020a0e68;
    src = c + 0xf0;
    tr = (char *)&data_ov021_02114a20[0];
    i = 0;
    obj = c;
    for (; i < 4; i++) {
        int ny;

        v[0] = *(int *)tr;
        ny = *(int *)(tr + 4);
        v[rv] = ny;
        v[2] = *(int *)(tr + 8);
        if (*(signed char *)(c + 0xc00 + 0x7a) == i) {
            if (*(unsigned char *)(c + 0xc7d) == 0) {
                v[rv] = ny - 0x1e000;
            }
        }
        data_020a0e68 = *(struct Matrix4x3 *)src;
        Vec3_Asr(vo, (void *)v, 3);
        Matrix4x3_ApplyInPlaceToTranslation(&data_020a0e68, vo[0], vo[rv],
                                            vo[2]);
        Matrix4x3_ApplyInPlaceToRotationY(&data_020a0e68,
                                          data_ov021_02114740[i]);
        *(struct Matrix4x3 *)(obj + 0x33c) = data_020a0e68;
        tr += 0xc;
        obj += 0x50;
    }

    rv = *(unsigned short *)(c + 0xc00 + 0x74);
    if (rv >= 0x2d)
        return rv;
    return rv & 1;   /* the mwcc asm veneer's whole body */
}
