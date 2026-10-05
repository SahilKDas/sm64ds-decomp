//cpp
// @symbol _ZN8SaveData8SaveFileEjP12FileSaveData
#include "SaveData.h"

/* SaveData::SaveFile(u32 fileID, FileSaveData* data) at 0x02013d14 -- static.
 *
 * Sets bit 0 of the word at data+0x4 -- marking the slot as in use -- then writes
 * the 0x44-byte block to cart, inverting SaveDataToCart's 0-is-success result.
 *
 * The `long long` cast is a codegen hack rather than meaning: it forces the
 * address into a register instead of being folded into the store. Removing it
 * changes the instruction. See plan-cpp-language-mode.md Phase 6.
 *
 * This file previously declared its own `struct SaveData` and a two-field
 * `FileSaveData`; both are retired in favour of the real header. That local
 * `FileSaveData { int _00; int _04; }` was also wrong about the size -- the block
 * is 0x44 bytes, as the call below has always said.
 */
#ifdef _MSC_VER
extern "C" u8 data_0209caa0[]; /* the open file, as in SaveCurrentFile */
#endif

int SaveData::SaveFile(u32 fileID, FileSaveData* data)
{
    int* ip = (int*)((char*)data + 4);
    *ip = *ip | 1;
#ifdef _MSC_VER
    /* PORT ONLY (the DS side above and below is byte for byte the ROM's). The
     * port's save states roll the game's copy of the star bytes back, and the
     * card is not rolled back with it, so a save of the open file that follows
     * could write fewer stars than the slot already holds. On the cartridge the
     * open file is read off the card and only ever added to, so this changes
     * nothing there: it ORs the stars already stored in this slot into the
     * record about to be written. The record's star bytes are +0x14..+0x31,
     * data_0209cab4 for the open file at 0x0209caa0.
     *
     * Only the open file (SaveCurrentFile passes data_0209caa0 itself). Any
     * other record is written unchanged: the file select's COPY writes the
     * source's bytes over a slot that may already hold stars, and the
     * cartridge leaves that slot an exact copy. */
    {
        if ((void*)data == (void*)data_0209caa0) {
            char stored[0x44];
            if (SaveData::ReadDataFromCart(stored, 0x44, fileID) == 0) {
                int i;
                for (i = 0x14; i < 0x32; i++)
                    ((char*)data)[i] |= stored[i];
            }
        }
    }
#endif
    if (SaveData::SaveDataToCart((char*)data, 0x44, fileID) == 0)
        return 1;
    return 0;
}
