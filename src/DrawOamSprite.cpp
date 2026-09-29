//cpp
struct OamAttr; struct Matrix2x2;
struct Base {
    virtual int v00(); virtual int v01(); virtual int v02(); virtual int v03();
    virtual int v04(); virtual int v05(); virtual int v06(); virtual int v07();
    virtual int v08(); virtual int v09(); virtual int v10(); virtual int v11();
    virtual int v12(); virtual int v13(); virtual int v14(); virtual int v15();
    virtual int v16(); virtual int v17(); virtual int v18(); virtual int v19();
    virtual int v20(); virtual int v21(); virtual int v22(); virtual int v23();
    virtual int v24(); virtual int v25(); virtual int m();
};
namespace OAM { void Render(bool draw, OamAttr *sprite, int x, int y, int palette, int priority, Matrix2x2 *matrix); }
extern "C" Base *data_ov004_020beb68;

extern "C" void DrawOamSprite(void *sprite, void *x, int y, void *matrix)
{
    Base *scene = data_ov004_020beb68;
    if (scene == 0)
        return;
    if (*(int*)((char*)scene + 0x4628) == 0 && scene->m() == 2) {
        if (*(unsigned short*)((char*)data_ov004_020beb68 + 0x4664) != 1)
            return;
        OAM::Render(false, (OamAttr*)sprite, (int)x, y, -1, -1, (Matrix2x2*)matrix);
        return;
    }
    OAM::Render(false, (OamAttr*)sprite, (int)x, y, -1, -1, (Matrix2x2*)matrix);
}
