//cpp
/* TextureTransformer -- Animation child driving BTA-file playback. Cartridge
   class: _ZTV18TextureTransformer at 0x0208e7c4 (two slots, the destructor
   pair); the cartridge RTTI names it dExtAnmTexSRT_c. TU claims
   0x0201587c..0x0201597c, the whole class run in delinks order. Written
   back-to-front: mwccarm emits .text in reverse source order under default
   deferred codegen. */
#include "TextureTransformer.h"

/* The cartridge spells this class dExtAnmTexSRT_c, so the compiler-emitted
   _ZTS18TextureTransformer / _ZTI18TextureTransformer / _ZTS9Animation /
   _ZTI9Animation records would be homeless under either spelling. Compiling
   with RTTI off emits no records at all; the vtable preamble's typeinfo word
   deadstrips with the rest of the data sections. */
#pragma RTTI off

extern "C" void func_02046b64(BMD_File *model, BTA_File *file);
extern "C" void func_020469e8(ModelComponents *model, BTA_File *file, int frame);
/* SetFile below and its SetAnimation call keep their mangled spellings:
   wall 6az (notes/mwccarm-codegen.md) homes class-typed by-value
   parameters that a body reads, and the real signature carries
   Fix12<int> -- passing one to the member declaration homes it to the
   caller's stack. The declarations in TextureTransformer.h and Animation.h
   are the real ones. */
extern "C" void _ZN9Animation12SetAnimationEti5Fix12IiEt(Animation *self, u16 numFrames, s32 flags, s32 speed, u16 startFrame);

// @symbol _ZN18TextureTransformerC1Ev
TextureTransformer::TextureTransformer()
{
    file = 0;
}

// @symbol _ZN18TextureTransformerD1Ev
TextureTransformer::~TextureTransformer()
{
}

// @symbol _ZN18TextureTransformer7SetFileER8BTA_Filei5Fix12IiEj
extern "C" void _ZN18TextureTransformer7SetFileER8BTA_Filei5Fix12IiEj(TextureTransformer *self, BTA_File *file, s32 flags, s32 speed, u16 startFrame)
{
    if (file == self->file) {
        self->SetFlags(flags);
        self->speed = speed;
    } else {
        self->file = file;
        _ZN9Animation12SetAnimationEti5Fix12IiEt(self, file->numFrames, flags, speed, startFrame);
    }
}

// @symbol _ZN18TextureTransformer6UpdateER15ModelComponents
void TextureTransformer::Update(ModelComponents &model)
{
    func_020469e8(&model, file, (u16)((u32)currFrame >> 12));
}

/* The ROM body is a 0xc long-call veneer (ldr ip, [pc]; bx ip); the
   registers pass through untouched, so the matched func_02046b64.c's
   own two-argument signature is the call surface. Prepare is static:
   no this. */
// @symbol _ZN18TextureTransformer7PrepareER8BMD_FileR8BTA_File
void TextureTransformer::Prepare(BMD_File &model, BTA_File &animFile)
{
    func_02046b64(&model, &animFile);
}
