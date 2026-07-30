#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"

void save_write(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);
void save_read(RDRAM_ARG PTR(void) rdram_address, uint32_t offset, uint32_t count);

// HH_TRACE_PFS=1 logs every Controller Pak call. The osPfs API is where a save
// either happens or silently does not, and the game's own save layer is
// dispatched through a function-pointer table, so static analysis cannot say
// which call it gave up on. This can.
static bool trace_pfs() {
    static const bool enabled = getenv("HH_TRACE_PFS") != nullptr;
    return enabled;
}

// The single file this pak presents, and the space around it.
//
// A real Controller Pak has 256 pages of 256 bytes; 5 are the header, label and
// two directory/inode copies, leaving 123 pages -- 31488 bytes -- for files.
// PFS_FILE_SIZE is what osPfsFileState reports for file 0, so free space has to
// be the rest of that budget, not the whole of it: a game that asks for free
// space and then allocates would otherwise be told it has room the pak does not
// have.
static constexpr uint32_t PFS_FILE_SIZE = 0x1000;
static constexpr uint32_t PFS_TOTAL_BYTES = 123 * 256;
static constexpr uint32_t PFS_FREE_BYTES = PFS_TOTAL_BYTES - PFS_FILE_SIZE;

// The identity of the one file this pak presents.
//
// Not invented and not inherited: these are the values Hybrid Heaven itself
// passes to osPfsFindFile, read straight off an HH_TRACE_PFS run --
// "osPfsFindFile company 4134 game 4E485645". 0x4E485645 is "NHVE", the ROM's
// own game code, and 0x4134 is "A4", Konami's licensee code.
//
// What was here before was 0x4E473545 ("NG5E") and 0x4A, which is Quest64's
// identity: the stub this file grew from was written for that game. A game that
// checks whether the file it found is its own would have rejected it. Nothing
// has been observed doing that check yet -- the game has only just started
// reaching the file-level calls at all -- so this is a correctness fix ahead of
// the evidence rather than a fix for an observed failure.
static constexpr uint32_t PFS_GAME_CODE = 0x4E485645;    // "NHVE"
static constexpr uint32_t PFS_COMPANY_CODE = 0x4134;     // "A4", Konami

// Fills in the three OSPfs fields Quest64-Recomp's pak.cpp sets, and no others.
//
// EXACTLY three, and the "no others" is load-bearing. A first attempt also wrote
// version, dir_size and the inode/dir page numbers -- plausible values, none of
// them read from a real pak -- and the game took an access violation at startup
// on a host address, having computed a pointer from numbers this file invented.
// Writing a field the game trusts is only safe when the value is real.
//
// HH_PFS_LEGACY_INIT=1 restores the write-nothing behaviour for both entry
// points, which is the A/B that identified this.
static void pfs_init_struct(uint8_t* rdram, PTR(void) pfs, PTR(void) queue, s32 channel) {
    MEM_W(0x00, pfs) = 1;              // status = PFS_INITIALIZED
    MEM_W(0x04, pfs) = (int32_t)queue; // queue
    MEM_W(0x08, pfs) = channel;        // channel
}

// osPfsInit(OSMesgQueue *queue, OSPfs *pfs, int channel).
//
// Hybrid Heaven calls this 28 times to osPfsInitPak's 10, so it is the dominant
// path and leaving it writing nothing is why populating osPfsInitPak alone did
// not clear "Physically damaged Controller Pak". Quest64-Recomp populates both.
extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_PFS_LEGACY_INIT") != nullptr;

    PTR(void) queue = _arg<0, PTR(void)>(rdram, ctx);
    PTR(void) pfs = _arg<1, PTR(void)>(rdram, ctx);
    s32 channel = _arg<2, s32>(rdram, ctx);

    if (!legacy) {
        pfs_init_struct(rdram, pfs, queue, channel);
    }

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsInit channel %d -> PFS_OK%s\n",
                channel, legacy ? " (legacy: struct untouched)" : ", status=1");
    }

    ctx->r2 = 0; // PFS_OK
}

// osPfsInitPak(OSMesgQueue *queue, OSPfs *pfs, int channel).
//
// This returned PFS_OK while writing NOTHING into the caller's OSPfs, and that is
// what made Hybrid Heaven put up "Physically damaged Controller Pak is inserted
// in Controller 1." The game's save layer keeps a 4-element OSPfs array (stride
// 0x68 at D_8005CE70, which is sizeof(OSPfs) exactly), calls this for each
// channel, and then reads the struct back. With status left at 0 the
// PFS_INITIALIZED bit is clear, so a pak that reported success looks broken --
// and the game never gets as far as osPfsFindFile, which is why a trace of a
// failed save showed twelve osPfsInitPak calls and nothing else.
//
// Quest64-Recomp hit the same wall and fixed it the same way: its pak.cpp sets
// queue, channel and status = 1 here. Goemon64Recomp's pak.cpp is byte-identical
// to the stub we inherited, so it has this bug too -- it simply never shows,
// because Goemon does not use the Controller Pak.
//
// Written through MEM_W/MEM_B rather than by assigning to an OSPfs* the way
// Quest64 does. A host-side struct write lands in native byte order, so a
// `status = 1` is read back by the guest as 0x01000000: still nonzero, which is
// why it passes a `status != 0` test and would fail a `status & PFS_INITIALIZED`
// one. MEM_W does the swap, so the guest sees the value actually intended.
//
// HH_PFS_LEGACY_INIT=1 restores the write-nothing behaviour, as the A/B.
extern "C" void osPfsInitPak_recomp(uint8_t * rdram, recomp_context* ctx) {
    static const bool legacy = getenv("HH_PFS_LEGACY_INIT") != nullptr;

    PTR(void) queue = _arg<0, PTR(void)>(rdram, ctx);
    PTR(void) pfs = _arg<1, PTR(void)>(rdram, ctx);
    s32 channel = _arg<2, s32>(rdram, ctx);

    if (legacy) {
        if (trace_pfs()) {
            fprintf(stderr, "[pfs] osPfsInitPak channel %d -> PFS_OK (legacy: struct untouched)\n",
                    channel);
        }
        ctx->r2 = 0;
        return;
    }

    pfs_init_struct(rdram, pfs, queue, channel);

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsInitPak channel %d -> PFS_OK, status=1\n", channel);
    }

    ctx->r2 = 0; // PFS_OK
}

// osPfsFreeBlocks(OSPfs *pfs, s32 *bytes_not_used).
//
// This used to return PFS_OK without writing the out parameter at all, so the
// caller read whatever was already in that stack slot. A game that checks free
// space before saving would then branch on uninitialised memory -- "Controller
// Pak full" on a pak with room, or worse, the reverse. The value is the space
// left after the one file osPfsFileState reports.
extern "C" void osPfsFreeBlocks_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(void) bytes_not_used = _arg<1, PTR(void)>(rdram, ctx);

    MEM_W(0, bytes_not_used) = (s32)PFS_FREE_BYTES;

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsFreeBlocks -> %u bytes free\n", PFS_FREE_BYTES);
    }

    ctx->r2 = 0; // PFS_OK
}

extern "C" void osPfsAllocateFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    // The out parameter is the 7th argument, at 0x18($sp) past the 4 register
    // args. A caller that allocates and then uses the returned file number would
    // otherwise read an uninitialised slot; file 0 is the only one that exists.
    PTR(void) file_no_out = MEM_W(0x18, ctx->r29);
    if (file_no_out != 0) {
        MEM_W(0, file_no_out) = 0;
    }
    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsAllocateFile -> PFS_OK, file 0\n");
    }
    ctx->r2 = 0; // PFS_OK
}

// Succeeds without deleting anything.
//
// This pak presents exactly one permanent file backed by the save buffer, so a
// delete has nothing to do -- but "nothing to do" is not the same as "report
// failure". It used to return PFS_ERR_INVALID, and a game that deletes and
// recreates its file in order to rewrite it would be stopped dead there, which
// looks exactly like "saving does nothing".
//
// PFS_OK is the answer the hardware would give for a file that exists, which
// this one does: osPfsFindFile reports it and osPfsFileState describes it. The
// data outliving the delete is a divergence, and a harmless one -- the caller's
// next move is osPfsAllocateFile, which also succeeds, and then a write that
// overwrites the buffer anyway.
//
// Still traced, because "the game deleted its save file" is worth seeing in a
// log even when the call is a no-op.
extern "C" void osPfsDeleteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsDeleteFile -> PFS_OK (no-op; the buffer is not cleared)\n");
    }
    ctx->r2 = 0; // PFS_OK
}

extern "C" void osPfsFileState_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    s32 file_no = _arg<1, s32>(rdram, ctx);
    PTR(void) state = _arg<2, PTR(void)>(rdram, ctx);

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsFileState file %d -> %s\n",
                file_no, file_no == 0 ? "PFS_OK" : "PFS_ERR_INVALID");
    }

    // Only the first file is valid.
    if (file_no == 0) {
        MEM_W(0, state) = PFS_FILE_SIZE; // file_size
        MEM_W(4, state) = PFS_GAME_CODE; // game_code
        MEM_H(8, state) = PFS_COMPANY_CODE; // company_code

        for (s32 i = 0; i < 4; i++) {
            MEM_B(10 + i, state) = 0; // ext_name
        }

        for (s32 i = 0; i < 16; i++) {
            MEM_B(14 + i, state) = 0; // game_name
        }

        ctx->r2 = 0;
    } else {
        ctx->r2 = 5; // PFS_ERR_INVALID
    }
}

// osPfsFindFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name,
//               u8 *ext_name, s32 *file_no).
//
// Six arguments: the last two arrive on the caller's stack at 0x10 and 0x14, and
// the sixth is a POINTER to where the found file number goes.
//
// That distinction was the bug. This used to do `MEM_W(20, ctx->r29) = 0`, which
// writes zero over the caller's argument slot -- the pointer itself -- and never
// touches the variable the caller is going to read. Hybrid Heaven's wrapper
// `func_80002DBC_39BC` passes a pointer to a stack byte and its caller
// `func_800031EC_3DEC` then does `lbu $a1, 0x26($sp)` and passes that to
// osPfsReadWriteFile, so the file number it read was whatever had been left in
// that slot: a trace of the first save attempt ever to get this far showed
// "osPfsReadWriteFile file 40".
//
// Same family as osPfsFreeBlocks and osPfsAllocateFile returning PFS_OK without
// writing their out-parameters, and the loudest member of it, because this one
// wrote to the wrong place rather than not at all.
extern "C" void osPfsFindFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(void) game_name = _arg<3, PTR(void)>(rdram, ctx);
    PTR(void) ext_name = MEM_W(16, ctx->r29);
    PTR(void) file_no_out = MEM_W(20, ctx->r29);

    if (trace_pfs()) {
        // The identity the game is looking for. This pak answers PFS_OK for any
        // of them, so a mismatch against what osPfsFileState reports cannot
        // itself block a save -- but it is the first thing to check if the game
        // decides the file is not its own.
        char name[17] = {};
        char ext[5] = {};
        for (s32 i = 0; i < 16; i++) {
            u8 c = game_name != 0 ? MEM_B(i, game_name) : 0;
            name[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        for (s32 i = 0; i < 4; i++) {
            u8 c = ext_name != 0 ? MEM_B(i, ext_name) : 0;
            ext[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        fprintf(stderr, "[pfs] osPfsFindFile company %04X game %08X name \"%s\" ext \"%s\""
                        " -> PFS_OK, file 0\n",
                (uint32_t)(_arg<1, u32>(rdram, ctx) & 0xFFFF),
                (uint32_t)_arg<2, u32>(rdram, ctx), name, ext);
    }

    if (file_no_out != 0) {
        MEM_W(0, file_no_out) = 0;
    }
    ctx->r2 = 0; // PFS_OK
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    s32 file_no = _arg<1, s32>(rdram, ctx);
    u8 flag = _arg<2, u8>(rdram, ctx);
    s32 offset = _arg<3, s32>(rdram, ctx);
    s32 size_in_bytes = MEM_W(16, ctx->r29);
    PTR(u8) data = MEM_W(20, ctx->r29);

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsReadWriteFile file %d %s offset %d size %d\n",
                file_no, flag == 0 ? "READ" : (flag == 1 ? "WRITE" : "?"),
                offset, size_in_bytes);
    }

    // File 0 is the only file this pak has, and osPfsFindFile / osPfsFileState
    // both say so, so a request for any other one is a caller error. Report it
    // as one rather than asserting: the assert here was compiled out by NDEBUG in
    // every build that has ever run, so it was documentation with an abort
    // attached for whoever eventually built without it.
    if (file_no != 0) {
        if (trace_pfs()) {
            fprintf(stderr, "[pfs] osPfsReadWriteFile file %d -> PFS_ERR_INVALID"
                            "  <-- only file 0 exists\n", file_no);
        }
        ctx->r2 = 5; // PFS_ERR_INVALID
        return;
    }

    if (flag == 0) {
        save_read(rdram, data, offset, size_in_bytes);
    } else if (flag == 1) {
        save_write(rdram, data, offset, size_in_bytes);
    }

    ctx->r2 = 0; // PFS_OK
}

extern "C" void osPfsChecker_recomp(uint8_t * rdram, recomp_context * ctx) {
    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsChecker -> PFS_OK\n");
    }
    ctx->r2 = 0; // PFS_OK
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Bit per channel; only controller 1 has a pak in this port.
    MEM_B(0, ctx->r5) = 1;
    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsIsPlug -> pattern 0b0001, PFS_OK\n");
    }
    ctx->r2 = 0; // PFS_OK
}

// osPfsNumFiles(OSPfs *pfs, s32 *max_files, s32 *files_used).
//
// Both out-parameters go through MEM_W, not through a host s32*. Assigning to a
// host pointer into rdram writes native byte order, so a 1 is read back by the
// guest as 0x01000000 -- which passes a `!= 0` test and fails a comparison
// against 1. It is the same trap that made an `OSPfs.status = 1` written host-side
// pass `status != 0` and fail `status & PFS_INITIALIZED`, which is why every write
// in this file goes through MEM_W/MEM_B.
//
// Nothing in Hybrid Heaven has been observed calling this, so the fix is
// unverified by anything but inspection. Upstream still has the bug.
extern "C" void osPfsNumFiles_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(void) max_files = _arg<1, PTR(void)>(rdram, ctx);
    PTR(void) files_used = _arg<2, PTR(void)>(rdram, ctx);

    if (max_files != 0) {
        MEM_W(0, max_files) = 1;
    }
    if (files_used != 0) {
        MEM_W(0, files_used) = 1;
    }

    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsNumFiles -> max 1, used 1\n");
    }

    _return<s32>(ctx, 0); // PFS_OK
}

extern "C" void osPfsRepairId_recomp(uint8_t * rdram, recomp_context * ctx) {
    if (trace_pfs()) {
        fprintf(stderr, "[pfs] osPfsRepairId -> PFS_OK"
                        "  <-- the game only repairs an id it thinks is BROKEN\n");
    }
    _return<s32>(ctx, 0); // PFS_OK
}
