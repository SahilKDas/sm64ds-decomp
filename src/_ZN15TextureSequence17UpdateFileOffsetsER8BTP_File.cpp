//cpp
// @symbol _ZN15TextureSequence17UpdateFileOffsetsER8BTP_File
#include "TextureSequence.h"

#define REBASE(offset) ((char *)(offset) + fileBase)

/* Keep each offset load and fixup in its own scope. This preserves the
   compiler's register allocation for the record loop. */

void TextureSequence::UpdateFileOffsets(BTP_File &file)
{
    int fileBase = (int)&file;
    int textureIndex = 0;
    if (file.unk_04) file.unk_04 = REBASE(file.unk_04);
    {
        char *textureRecord = file.unk_04;
        while (textureIndex < (int)file.numTexRecords) {
            {
                int offset = *(int *)(textureRecord + 4);
                if (offset)
                    *(int *)(textureRecord + 4) = offset + fileBase;
            }
            textureIndex++;
            textureRecord += 8;
        }
    }
    if (file.unk_0c) file.unk_0c = REBASE(file.unk_0c);
    {
        char *paletteRecord = file.unk_0c;
        int paletteIndex = 0;
        while (paletteIndex < (int)file.numPalRecords) {
            {
                int offset = *(int *)(paletteRecord + 4);
                if (offset)
                    *(int *)(paletteRecord + 4) = offset + fileBase;
            }
            paletteIndex++;
            paletteRecord += 8;
        }
    }
    if (file.unk_10) file.unk_10 = REBASE(file.unk_10);
    if (file.unk_14) file.unk_14 = REBASE(file.unk_14);
    if (file.unk_18) file.unk_18 = REBASE(file.unk_18);
    if (file.unk_20) file.unk_20 = REBASE(file.unk_20);
    {
        char *sequenceRecord = file.unk_20;
        int sequenceIndex = 0;
        while (sequenceIndex < (int)file.numSeqRecords) {
            {
                int offset = *(int *)(sequenceRecord + 4);
                if (offset)
                    *(int *)(sequenceRecord + 4) = offset + fileBase;
            }
            sequenceIndex++;
            sequenceRecord += 0xc;
        }
    }
}
