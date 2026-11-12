/* ux_mixer.c -- UltraX: the EAD audio microcode's commands as functions
 * (ux_mixer.h), on the CPU.
 *
 * This is 2Ship2Harkinian's mm/2s2h/mixer.c (CC0: github.com/2ship2harkinian/
 * 2ship2harkinian), itself grown from the Super Mario 64 PC port's: the same
 * code, with its Opus decoder and its SSE2/NEON/AVX copies of aMix left out
 * (the portable versions here compute the same; the Xbox's Pentium III has no
 * SSE2) and counters for the log. Fixes to it belong upstream first.
 *
 * "DMEM" is rspa.buf, addressed as the microcode addresses it (from 0x330).
 */
#include <stdbool.h>
#include <string.h>

/* the names mixer.c uses, as ux_mixer.h's own types (no <stdint.h> here) */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;

#include "ultrax/ux.h"
#include "ultrax/ux_mixer.h"

/* UltraX: on x86 (the Xbox's Pentium III) the envelope mixer, aMix, aAddMixer
   and aFilter are MMX, as inline assembly (clang's MMX intrinsics are made of
   SSE2, which a Pentium III lacks). Each gives exactly what mixer.c's C does,
   which stays (the ...Ref functions) for other hosts and is what
   tests/mixer_test.c checks them against. Timed against the C on the same
   calls on a console, the envelope mixer took a third of the C's time, the
   filter two thirds, the mixes a little less; ADPCM decoding as eight dot
   products per half frame (pmaddwd) was no faster than the C and is the C. */
#if defined(__i386__) || defined(__x86_64__)
#define UX_MIXER_MMX 1
#else
#define UX_MIXER_MMX 0
#endif

/* the microcode's flags and state blocks, as PR/abi.h has them */
#define A_INIT 0x01
#define A_LOOP 0x02
typedef int16_t ADPCM_STATE[16];
typedef int16_t RESAMPLE_STATE[16];

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

/* UltraX: all of the RSP's DMEM, 4 KB from 0. mixer.c has 0xC80 bytes from 0x330
   (to 0xFB0), but Majora's Mask's commands reach past 0xFB0, and there the
   writes went on into whatever the linker put next -- on the Xbox, the audio
   driver's gAudioCtx (garbled reverb, then a crash). A guard follows, and
   ux_mixer_log_stats says if anything ever reaches it. */
#define DMEM_BUF_SIZE 0x1000
#define DMEM_GUARD 0x400
/* Lus-X: addresses wrap at 4 KB, as the RSP's 12-bit DMEM addresses do: a bad
   address (aSetBuffer was given 0xECC0) then lands in DMEM, not 59 KB past it */
#define DMEM_ADDR(a) ((unsigned)(a) & (DMEM_BUF_SIZE - 1))
#define BUF_U8(a) (rspa.buf.as_u8 + DMEM_ADDR(a))
#define BUF_S16(a) (rspa.buf.as_s16 + DMEM_ADDR(a) / sizeof(int16_t))

static struct {
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    uint16_t vol[2];
    uint16_t rate[2];
    uint16_t vol_wet;
    uint16_t rate_wet;

    ADPCM_STATE* adpcm_loop_state;

    int16_t adpcm_table[8][2][8];

    uint16_t filter_count;
    int16_t filter[8];

    union {
        int16_t as_s16[DMEM_BUF_SIZE / sizeof(int16_t)];
        uint8_t as_u8[DMEM_BUF_SIZE];
    } buf;
    uint8_t guard[DMEM_GUARD]; /* UltraX: see DMEM_BUF_SIZE */
} rspa;

static int16_t resample_table[64][4] = {
    { 0x0c39, 0x66ad, 0x0d46, 0xffdf }, { 0x0b39, 0x6696, 0x0e5f, 0xffd8 }, { 0x0a44, 0x6669, 0x0f83, 0xffd0 },
    { 0x095a, 0x6626, 0x10b4, 0xffc8 }, { 0x087d, 0x65cd, 0x11f0, 0xffbf }, { 0x07ab, 0x655e, 0x1338, 0xffb6 },
    { 0x06e4, 0x64d9, 0x148c, 0xffac }, { 0x0628, 0x643f, 0x15eb, 0xffa1 }, { 0x0577, 0x638f, 0x1756, 0xff96 },
    { 0x04d1, 0x62cb, 0x18cb, 0xff8a }, { 0x0435, 0x61f3, 0x1a4c, 0xff7e }, { 0x03a4, 0x6106, 0x1bd7, 0xff71 },
    { 0x031c, 0x6007, 0x1d6c, 0xff64 }, { 0x029f, 0x5ef5, 0x1f0b, 0xff56 }, { 0x022a, 0x5dd0, 0x20b3, 0xff48 },
    { 0x01be, 0x5c9a, 0x2264, 0xff3a }, { 0x015b, 0x5b53, 0x241e, 0xff2c }, { 0x0101, 0x59fc, 0x25e0, 0xff1e },
    { 0x00ae, 0x5896, 0x27a9, 0xff10 }, { 0x0063, 0x5720, 0x297a, 0xff02 }, { 0x001f, 0x559d, 0x2b50, 0xfef4 },
    { 0xffe2, 0x540d, 0x2d2c, 0xfee8 }, { 0xffac, 0x5270, 0x2f0d, 0xfedb }, { 0xff7c, 0x50c7, 0x30f3, 0xfed0 },
    { 0xff53, 0x4f14, 0x32dc, 0xfec6 }, { 0xff2e, 0x4d57, 0x34c8, 0xfebd }, { 0xff0f, 0x4b91, 0x36b6, 0xfeb6 },
    { 0xfef5, 0x49c2, 0x38a5, 0xfeb0 }, { 0xfedf, 0x47ed, 0x3a95, 0xfeac }, { 0xfece, 0x4611, 0x3c85, 0xfeab },
    { 0xfec0, 0x4430, 0x3e74, 0xfeac }, { 0xfeb6, 0x424a, 0x4060, 0xfeaf }, { 0xfeaf, 0x4060, 0x424a, 0xfeb6 },
    { 0xfeac, 0x3e74, 0x4430, 0xfec0 }, { 0xfeab, 0x3c85, 0x4611, 0xfece }, { 0xfeac, 0x3a95, 0x47ed, 0xfedf },
    { 0xfeb0, 0x38a5, 0x49c2, 0xfef5 }, { 0xfeb6, 0x36b6, 0x4b91, 0xff0f }, { 0xfebd, 0x34c8, 0x4d57, 0xff2e },
    { 0xfec6, 0x32dc, 0x4f14, 0xff53 }, { 0xfed0, 0x30f3, 0x50c7, 0xff7c }, { 0xfedb, 0x2f0d, 0x5270, 0xffac },
    { 0xfee8, 0x2d2c, 0x540d, 0xffe2 }, { 0xfef4, 0x2b50, 0x559d, 0x001f }, { 0xff02, 0x297a, 0x5720, 0x0063 },
    { 0xff10, 0x27a9, 0x5896, 0x00ae }, { 0xff1e, 0x25e0, 0x59fc, 0x0101 }, { 0xff2c, 0x241e, 0x5b53, 0x015b },
    { 0xff3a, 0x2264, 0x5c9a, 0x01be }, { 0xff48, 0x20b3, 0x5dd0, 0x022a }, { 0xff56, 0x1f0b, 0x5ef5, 0x029f },
    { 0xff64, 0x1d6c, 0x6007, 0x031c }, { 0xff71, 0x1bd7, 0x6106, 0x03a4 }, { 0xff7e, 0x1a4c, 0x61f3, 0x0435 },
    { 0xff8a, 0x18cb, 0x62cb, 0x04d1 }, { 0xff96, 0x1756, 0x638f, 0x0577 }, { 0xffa1, 0x15eb, 0x643f, 0x0628 },
    { 0xffac, 0x148c, 0x64d9, 0x06e4 }, { 0xffb6, 0x1338, 0x655e, 0x07ab }, { 0xffbf, 0x11f0, 0x65cd, 0x087d },
    { 0xffc8, 0x10b4, 0x6626, 0x095a }, { 0xffd0, 0x0f83, 0x6669, 0x0a44 }, { 0xffd8, 0x0e5f, 0x6696, 0x0b39 },
    { 0xffdf, 0x0d46, 0x66ad, 0x0c39 }
};


static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

/* for ux_mixer_log_stats; and the time the heavy commands took, by kind, in
   the processor's cycles as the graphics profile counts them: the host clock
   (QueryPerformanceCounter) is a slow hardware timer on the Xbox, and read
   twice a command it was much of what it timed */
static uint32_t sFrames4, sFrames2, sVoices;
enum { T_DECODE, T_RESAMPLE, T_ENVELOPE, T_MIX, T_FILTER, T_KINDS };
static ux_u64 sTicks[T_KINDS];

/* Lus-X: the per-command checks and timings (two rdtsc and a 1 KB guard scan a command: ~1-3% of the
   core) are off; DMEM_ADDR's wrap and aSetBuffer's clamp are the protection that stays */
#define LUSX_MIXER_CHECKS 0

static inline ux_u64 mixer_cycles(void) {
#if !LUSX_MIXER_CHECKS
    return 0;
#elif UX_MIXER_MMX
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ux_u64)hi << 32) | lo;
#else
    return ux_host_ticks();
#endif
}

/* the guard after DMEM: which bytes were written (it is kept 0xA5), and the
   last 0x50 bytes of DMEM mixer.c did not have (for the log only) */
static int guard_hits(void) {
    static int sInit;
    int i, n = 0;

    if (!sInit) {
        memset(rspa.guard, 0xA5, sizeof(rspa.guard));
        sInit = 1;
        return 0;
    }
    for (i = 0; i < DMEM_GUARD; i++) {
        n += rspa.guard[i] != 0xA5;
    }
    memset(rspa.guard, 0xA5, sizeof(rspa.guard));
    return n;
}

/* Lus-X: decoded sound overwrote static memory past rspa (2Ship's effect context, 17-56 KB on). After
   each command that writes DMEM, the guard is looked at; the first command that wrote past DMEM is
   logged with its arguments and the buffer state set by aSetBuffer. */
static void check_guard(const char* name, unsigned a, unsigned b, unsigned c) {
    static int sArmed, sReported;
    int i;

#if !LUSX_MIXER_CHECKS
    return;
#endif
    if (!sArmed) {
        memset(rspa.guard, 0xA5, sizeof(rspa.guard));
        sArmed = 1;
        return;
    }
    if (sReported) {
        return;
    }
    for (i = 0; i < DMEM_GUARD && rspa.guard[i] == 0xA5; i++) {
    }
    if (i < DMEM_GUARD) {
        sReported = 1;
        ux_host_log("lus-x: mixer: %s(0x%X, 0x%X, 0x%X) wrote past DMEM (guard byte %d); in 0x%X out 0x%X "
                    "nbytes 0x%X",
                    name, a, b, c, i, (unsigned)rspa.in, (unsigned)rspa.out, (unsigned)rspa.nbytes);
    }
}

/* Lus-X: and a command given a DMEM address past DMEM writes beyond the guard without touching it (the
   effect context is DMEM-relative 0x4264..0xDB44): every address a command takes, checked first */
static void check_addrs(const char* name, unsigned a, unsigned b, unsigned c) {
    static int sReported;

#if !LUSX_MIXER_CHECKS
    return;
#endif
    if (!sReported && (a >= DMEM_BUF_SIZE || b >= DMEM_BUF_SIZE || c >= DMEM_BUF_SIZE)) {
        sReported = 1;
        ux_host_log("lus-x: mixer: %s given an address past DMEM: 0x%X 0x%X 0x%X (in 0x%X out 0x%X nbytes 0x%X)",
                    name, a, b, c, (unsigned)rspa.in, (unsigned)rspa.out, (unsigned)rspa.nbytes);
    }
}

void ux_mixer_log_stats(void) {
    static ux_u64 sLastTicks, sLastCycles;
    int past = guard_hits();
    ux_u64 t = ux_host_ticks(), c = mixer_cycles();
    double ms = 0;

    /* cycles to milliseconds, by the host clock over the same time */
    if (sLastTicks != 0 && t > sLastTicks && c > sLastCycles) {
        ms = (double)(t - sLastTicks) * 1000.0 / (double)ux_host_tick_rate() / (double)(c - sLastCycles);
    }
    sLastTicks = t;
    sLastCycles = c;
    ux_host_log("ultrax: mixer: ADPCM frames %u (4-bit) %u (2-bit), %u voice mixes; %.0f ms decoding, %.0f ms "
                "resampling, %.0f ms in envelopes, %.0f ms mixing, %.0f ms filtering%s", (unsigned)sFrames4,
                (unsigned)sFrames2, (unsigned)sVoices, (double)sTicks[T_DECODE] * ms,
                (double)sTicks[T_RESAMPLE] * ms, (double)sTicks[T_ENVELOPE] * ms, (double)sTicks[T_MIX] * ms,
                (double)sTicks[T_FILTER] * ms, past ? "; WROTE PAST DMEM" : "");
    if (past) {
        ux_host_log("ultrax: mixer: %d bytes past the end of DMEM were written", past);
    }
    sFrames4 = sFrames2 = sVoices = 0;
    memset(sTicks, 0, sizeof(sTicks));
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    check_addrs("aClearBuffer", addr, 0, 0);
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
    check_guard("aClearBuffer", addr, nbytes, 0);
}

void aLoadBufferImpl(const void* source_addr, uint16_t dest_addr, uint16_t nbytes) {
    check_addrs("aLoadBuffer", dest_addr, 0, 0);
#if __SANITIZE_ADDRESS__
    for (size_t i = 0; i < ROUND_DOWN_16(nbytes); i++) {
        BUF_U8(dest_addr)[i] = ((const unsigned char*)source_addr)[i];
    }
#else
    memcpy(BUF_U8(dest_addr), source_addr, ROUND_DOWN_16(nbytes));
#endif
    check_guard("aLoadBuffer", (unsigned)(unsigned long)source_addr, dest_addr, nbytes);
}

void aSaveBufferImpl(uint16_t source_addr, int16_t* dest_addr, uint16_t nbytes) {
    /* Lus-X: a save into the static memory after rspa (where the overwritten effect context is), not
       into an audio buffer: logged once */
    static int sReported;
    const char* lo = (const char*)&rspa;

    if (LUSX_MIXER_CHECKS && !sReported && (const char*)dest_addr + ROUND_DOWN_16(nbytes) > lo &&
        (const char*)dest_addr < lo + 0x20000) {
        sReported = 1;
        ux_host_log("lus-x: mixer: aSaveBuffer(0x%X, %p, 0x%X) writes into static memory (rspa at %p)",
                    (unsigned)source_addr, (void*)dest_addr, (unsigned)nbytes, (void*)lo);
    }
    memcpy(dest_addr, BUF_S16(source_addr), ROUND_DOWN_16(nbytes));
}

void aLoadADPCMImpl(int num_entries_times_16, const int16_t* book_source_addr) {
    memcpy(rspa.adpcm_table, book_source_addr, num_entries_times_16);
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    check_addrs("aSetBuffer", in, out, (unsigned)out + nbytes > DMEM_BUF_SIZE + DMEM_GUARD ? 0xFFFF : 0);
    rspa.in = DMEM_ADDR(in);
    rspa.out = DMEM_ADDR(out);
    /* and the decode/resample it sets up stays within DMEM and its guard (+0x40: the state written first) */
    if (rspa.out + nbytes + 0x40 > DMEM_BUF_SIZE + DMEM_GUARD) {
        nbytes = (DMEM_BUF_SIZE + DMEM_GUARD - 0x40 - rspa.out) & ~0x1f;
    }
    rspa.nbytes = nbytes;
}

void aInterleaveImpl(uint16_t dest, uint16_t left, uint16_t right, uint16_t c) {
    check_addrs("aInterleave", dest, left, right);
    int count = ROUND_UP_8(c) / sizeof(int16_t) / 4;
    int16_t* l = BUF_S16(left);
    int16_t* r = BUF_S16(right);
    int16_t* d = BUF_S16(dest);
    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;
        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        --count;
    }
    check_guard("aInterleave", dest, left, right);
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    check_addrs("aDMEMMove", in_addr, out_addr, (unsigned)out_addr + nbytes > DMEM_BUF_SIZE + DMEM_GUARD ? 0xFFFF : 0);
    nbytes = ROUND_UP_16(nbytes);
    memmove(BUF_U8(out_addr), BUF_U8(in_addr), nbytes);
    check_guard("aDMEMMove", in_addr, out_addr, nbytes);
}

void aSetLoopImpl(int16_t* adpcm_loop_state) {
    rspa.adpcm_loop_state = (ADPCM_STATE*)adpcm_loop_state;
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    ux_u64 t0 = mixer_cycles();
    uint8_t* in = BUF_U8(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;          // should be in 0..12 or 0..14
        int table_index = *in++ & 0xf; // should be in 0..7
        int16_t(*tbl)[8] = rspa.adpcm_table[table_index];
        int i;

        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j, k;
            if (flags & 4) {
                for (j = 0; j < 2; j++) {
                    ins[j * 4] = (((*in >> 6) << 30) >> 30) << shift;
                    ins[j * 4 + 1] = ((((*in >> 4) & 0x3) << 30) >> 30) << shift;
                    ins[j * 4 + 2] = ((((*in >> 2) & 0x3) << 30) >> 30) << shift;
                    ins[j * 4 + 3] = (((*in++ & 0x3) << 30) >> 30) << shift;
                }
            } else {
                for (j = 0; j < 4; j++) {
                    ins[j * 2] = (((*in >> 4) << 28) >> 28) << shift;
                    ins[j * 2 + 1] = (((*in++ & 0xf) << 28) >> 28) << shift;
                }
            }
            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
        nbytes -= 16 * sizeof(int16_t);
        if (flags & 4) {
            sFrames2++;
        } else {
            sFrames4++;
        }
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
    sTicks[T_DECODE] += mixer_cycles() - t0;
    check_guard("aADPCMdec", flags, 0, 0);
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    ux_u64 t0 = mixer_cycles();
    int16_t tmp[16];
    int16_t* in_initial = BUF_S16(rspa.in);
    int16_t* in = in_initial;
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator;
    int i;
    int16_t* tbl;
    int32_t sample;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

    do {
        for (i = 0; i < 8; i++) {
            tbl = resample_table[pitch_accumulator * 64 >> 16];
            sample = ((in[0] * tbl[0] + 0x4000) >> 15) + ((in[1] * tbl[1] + 0x4000) >> 15) +
                     ((in[2] * tbl[2] + 0x4000) >> 15) + ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);

            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
    sTicks[T_RESAMPLE] += mixer_cycles() - t0;
    check_guard("aResample", flags, pitch, 0);
}

void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet, uint16_t rate_left, uint16_t rate_right) {
    rspa.vol_wet = (uint16_t)(initial_vol_wet << 8);
    rspa.rate_wet = rate_wet;
    rspa.rate[0] = rate_left;
    rspa.rate[1] = rate_right;
}

void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right) {
    rspa.vol[0] = initial_vol_left;
    rspa.vol[1] = initial_vol_right;
}

static void aEnvMixerRef(uint16_t in_addr, uint16_t n_samples, bool swap_reverb, bool neg_3, bool neg_2,
                         bool neg_left, bool neg_right, int32_t wet_dry_addr, uint32_t unk) {
    int16_t* in = BUF_S16(in_addr);
    int16_t* dry[2] = { BUF_S16(((wet_dry_addr >> 24) & 0xFF) << 4), BUF_S16(((wet_dry_addr >> 16) & 0xFF) << 4) };
    int16_t* wet[2] = { BUF_S16(((wet_dry_addr >> 8) & 0xFF) << 4), BUF_S16(((wet_dry_addr)&0xFF) << 4) };
    int16_t negs[4] = { neg_left ? -1 : 0, neg_right ? -1 : 0, neg_3 ? -4 : 0, neg_2 ? -2 : 0 };
    int swapped[2] = { swap_reverb ? 1 : 0, swap_reverb ? 0 : 1 };
    int n = ROUND_UP_16(n_samples);

    uint16_t vols[2] = { rspa.vol[0], rspa.vol[1] };

    sVoices++;
    uint16_t rates[2] = { rspa.rate[0], rspa.rate[1] };
    uint16_t vol_wet = rspa.vol_wet;
    uint16_t rate_wet = rspa.rate_wet;

    do {
        for (int i = 0; i < 8; i++) {
            int16_t samples[2] = { *in, *in };
            in++;
            for (int j = 0; j < 2; j++) {
                samples[j] = (samples[j] * vols[j] >> 16) ^ negs[j];
            }
            for (int j = 0; j < 2; j++) {
                *dry[j] = clamp16(*dry[j] + samples[j]);
                dry[j]++;
                *wet[j] = clamp16(*wet[j] + ((samples[swapped[j]] * vol_wet >> 16) ^ negs[2 + j]));
                wet[j]++;
            }
        }
        vols[0] += rates[0];
        vols[1] += rates[1];
        vol_wet += rate_wet;

        n -= 8;
    } while (n > 0);
}

#if UX_MIXER_MMX
/* whether [a, a + n) and [b, b + n) share a byte */
static inline int overlap(const void* a, const void* b, int n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;

    return p < q + n && q < p + n;
}

/* The envelope mixer's constants for its assembly, four copies of each
   word: the left, right and wet volumes to start from, their ramps, the
   negs (left, right, wa's, wb's). The audio thread's alone, as rspa is. */
static struct {
    uint32_t v0[2], v1[2], vw[2], r0[2], r1[2], rw[2], n0[2], n1[2], na[2], nb[2];
} sEnv __attribute__((aligned(8)));

/* Four samples of the envelope mixer at byte offset o; mm5-mm7 hold the
   left, right and wet volumes. A volume is unsigned, so (s * vol) >> 16 is
   pmulhw's signed high half plus s where vol's top bit is set (psraw $15
   makes that mask). Then ^ neg, and paddsw is clamp16(a + b). x0 and x1 (the
   volumed left and right) go to the dry buffers and, at the wet volume, to
   wa and wb. */
#define ENV_GROUP(o)                                                                                       \
    "movq " #o "(%[in]), %%mm0\n\t"                                                                        \
    "movq %%mm0, %%mm1\n\t" "pmulhw %%mm5, %%mm1\n\t"                                                      \
    "movq %%mm5, %%mm3\n\t" "psraw $15, %%mm3\n\t" "pand %%mm0, %%mm3\n\t" "paddw %%mm3, %%mm1\n\t"        \
    "pxor %[n0], %%mm1\n\t"                                                                                \
    "movq %%mm0, %%mm2\n\t" "pmulhw %%mm6, %%mm2\n\t"                                                      \
    "movq %%mm6, %%mm3\n\t" "psraw $15, %%mm3\n\t" "pand %%mm3, %%mm0\n\t" "paddw %%mm0, %%mm2\n\t"        \
    "pxor %[n1], %%mm2\n\t"                                                                                \
    "movq " #o "(%[d0]), %%mm3\n\t" "paddsw %%mm1, %%mm3\n\t" "movq %%mm3, " #o "(%[d0])\n\t"               \
    "movq " #o "(%[d1]), %%mm3\n\t" "paddsw %%mm2, %%mm3\n\t" "movq %%mm3, " #o "(%[d1])\n\t"               \
    "movq %%mm7, %%mm4\n\t" "psraw $15, %%mm4\n\t"                                                         \
    "movq %%mm1, %%mm3\n\t" "pmulhw %%mm7, %%mm3\n\t" "pand %%mm4, %%mm1\n\t" "paddw %%mm1, %%mm3\n\t"     \
    "pxor %[na], %%mm3\n\t" "paddsw " #o "(%[wa]), %%mm3\n\t" "movq %%mm3, " #o "(%[wa])\n\t"              \
    "movq %%mm2, %%mm3\n\t" "pmulhw %%mm7, %%mm3\n\t" "pand %%mm4, %%mm2\n\t" "paddw %%mm2, %%mm3\n\t"     \
    "pxor %[nb], %%mm3\n\t" "paddsw " #o "(%[wb]), %%mm3\n\t" "movq %%mm3, " #o "(%[wb])\n\t"

/* the whole mix one loop of assembly: the volumes stay in mm5-mm7, ramped by
   paddw (a uint16 wraps as the C's does) every eight samples */
static void aEnvMixerMmx(uint16_t in_addr, uint16_t n_samples, bool swap_reverb, bool neg_3, bool neg_2,
                         bool neg_left, bool neg_right, int32_t wet_dry_addr, uint32_t unk) {
    int16_t* in = BUF_S16(in_addr);
    int16_t* d0 = BUF_S16(((wet_dry_addr >> 24) & 0xFF) << 4);
    int16_t* d1 = BUF_S16(((wet_dry_addr >> 16) & 0xFF) << 4);
    int16_t* w0 = BUF_S16(((wet_dry_addr >> 8) & 0xFF) << 4);
    int16_t* w1 = BUF_S16(((wet_dry_addr)&0xFF) << 4);
    int n = ROUND_UP_16(n_samples);
    int blocks = n > 0 ? n / 8 : 1; /* a do-while: one at least */
    int bytes = blocks * 8 * (int)sizeof(int16_t);
    /* wet[j] takes samples[swapped[j]] with negs[2 + j]: x0 goes to wa, x1 to wb */
    int16_t* wa = swap_reverb ? w1 : w0;
    int16_t* wb = swap_reverb ? w0 : w1;
    uint32_t na = swap_reverb ? (neg_2 ? 0xFFFE : 0) : (neg_3 ? 0xFFFC : 0);
    uint32_t nb = swap_reverb ? (neg_3 ? 0xFFFC : 0) : (neg_2 ? 0xFFFE : 0);

    /* the C goes sample by sample: buffers that share bytes would mix differently four at a time */
    if (overlap(in, d0, bytes) || overlap(in, d1, bytes) || overlap(in, w0, bytes) || overlap(in, w1, bytes) ||
        overlap(d0, d1, bytes) || overlap(d0, w0, bytes) || overlap(d0, w1, bytes) || overlap(d1, w0, bytes) ||
        overlap(d1, w1, bytes) || overlap(w0, w1, bytes)) {
        aEnvMixerRef(in_addr, n_samples, swap_reverb, neg_3, neg_2, neg_left, neg_right, wet_dry_addr, unk);
        return;
    }
    sVoices++;
    sEnv.v0[0] = sEnv.v0[1] = rspa.vol[0] * 0x10001u;
    sEnv.v1[0] = sEnv.v1[1] = rspa.vol[1] * 0x10001u;
    sEnv.vw[0] = sEnv.vw[1] = rspa.vol_wet * 0x10001u;
    sEnv.r0[0] = sEnv.r0[1] = rspa.rate[0] * 0x10001u;
    sEnv.r1[0] = sEnv.r1[1] = rspa.rate[1] * 0x10001u;
    sEnv.rw[0] = sEnv.rw[1] = rspa.rate_wet * 0x10001u;
    sEnv.n0[0] = sEnv.n0[1] = neg_left ? 0xFFFFFFFFu : 0;
    sEnv.n1[0] = sEnv.n1[1] = neg_right ? 0xFFFFFFFFu : 0;
    sEnv.na[0] = sEnv.na[1] = na * 0x10001u;
    sEnv.nb[0] = sEnv.nb[1] = nb * 0x10001u;
    __asm__ volatile("movq %[v0], %%mm5\n\t"
                     "movq %[v1], %%mm6\n\t"
                     "movq %[vw], %%mm7\n\t"
                     "1:\n\t"
                     ENV_GROUP(0) ENV_GROUP(8)
                     "paddw %[r0], %%mm5\n\t"
                     "paddw %[r1], %%mm6\n\t"
                     "paddw %[rw], %%mm7\n\t"
                     "add $16, %[in]\n\t"
                     "add $16, %[d0]\n\t"
                     "add $16, %[d1]\n\t"
                     "add $16, %[wa]\n\t"
                     "add $16, %[wb]\n\t"
                     "dec %[n]\n\t"
                     "jnz 1b\n\t"
                     "emms\n\t"
                     : [in] "+r"(in), [d0] "+r"(d0), [d1] "+r"(d1), [wa] "+r"(wa), [wb] "+r"(wb), [n] "+r"(blocks)
                     : [v0] "m"(sEnv.v0), [v1] "m"(sEnv.v1), [vw] "m"(sEnv.vw), [r0] "m"(sEnv.r0),
                       [r1] "m"(sEnv.r1), [rw] "m"(sEnv.rw), [n0] "m"(sEnv.n0), [n1] "m"(sEnv.n1),
                       [na] "m"(sEnv.na), [nb] "m"(sEnv.nb)
                     : "memory", "cc", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7");
}
#endif

void aEnvMixerImpl(uint16_t in_addr, uint16_t n_samples, bool swap_reverb, bool neg_3, bool neg_2, bool neg_left,
                   bool neg_right, int32_t wet_dry_addr, uint32_t unk) {
    ux_u64 t0 = mixer_cycles();
    check_addrs("aEnvMixer", in_addr, (unsigned)in_addr + n_samples * 2 > DMEM_BUF_SIZE ? 0xFFFF : 0, 0);

#if UX_MIXER_MMX
    aEnvMixerMmx(in_addr, n_samples, swap_reverb, neg_3, neg_2, neg_left, neg_right, wet_dry_addr, unk);
#else
    aEnvMixerRef(in_addr, n_samples, swap_reverb, neg_3, neg_2, neg_left, neg_right, wet_dry_addr, unk);
#endif
    sTicks[T_ENVELOPE] += mixer_cycles() - t0;
    check_guard("aEnvMixer", in_addr, n_samples, wet_dry_addr);
}

static void aMixImplRef(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(ROUND_DOWN_16(count << 4));
    int16_t* in = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int i;
    int32_t sample;

    if (gain == -0x8000) {
        while (nbytes > 0) {
            for (i = 0; i < 16; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
            nbytes -= 16 * sizeof(int16_t);
        }
    }

    while (nbytes > 0) {
        for (i = 0; i < 16; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }

        nbytes -= 16 * sizeof(int16_t);
    }
}

#if UX_MIXER_MMX
/* out = clamp16((out * 0x7FFF + in * gain + 0x4000) >> 15), four at a time:
   (out, in) pairs pmaddwd'd with (0x7FFF, gain) are the exact sums (they
   cannot reach 2^31), and packssdw is the clamp16. A gain of -0x8000 is
   clamp16(out - in), psubsw. */
static void aMixImplMmx(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(ROUND_DOWN_16(count << 4));
    int16_t* in = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int groups = nbytes / 8;
    uint32_t c[4] __attribute__((aligned(8)));

    if (groups <= 0) {
        return;
    }
    if (in != out && overlap(in, out, nbytes)) {
        aMixImplRef(count, gain, in_addr, out_addr); /* sample by sample */
        return;
    }
    if (gain == -0x8000) {
        __asm__ volatile("1:\n\t"
                         "movq (%[out]), %%mm0\n\t"
                         "psubsw (%[in]), %%mm0\n\t"
                         "movq %%mm0, (%[out])\n\t"
                         "add $8, %[in]\n\t"
                         "add $8, %[out]\n\t"
                         "dec %[n]\n\t"
                         "jnz 1b\n\t"
                         "emms\n\t"
                         : [in] "+r"(in), [out] "+r"(out), [n] "+r"(groups)
                         :
                         : "memory", "cc", "mm0");
        return;
    }
    c[0] = c[1] = ((uint32_t)(uint16_t)gain << 16) | 0x7FFF;
    c[2] = c[3] = 0x4000;
    __asm__ volatile("movq 0(%[c]), %%mm6\n\t"
                     "movq 8(%[c]), %%mm7\n\t"
                     "1:\n\t"
                     "movq (%[out]), %%mm0\n\t"
                     "movq (%[in]), %%mm1\n\t"
                     "movq %%mm0, %%mm2\n\t"
                     "punpcklwd %%mm1, %%mm0\n\t"
                     "punpckhwd %%mm1, %%mm2\n\t"
                     "pmaddwd %%mm6, %%mm0\n\t"
                     "pmaddwd %%mm6, %%mm2\n\t"
                     "paddd %%mm7, %%mm0\n\t"
                     "paddd %%mm7, %%mm2\n\t"
                     "psrad $15, %%mm0\n\t"
                     "psrad $15, %%mm2\n\t"
                     "packssdw %%mm2, %%mm0\n\t"
                     "movq %%mm0, (%[out])\n\t"
                     "add $8, %[in]\n\t"
                     "add $8, %[out]\n\t"
                     "dec %[n]\n\t"
                     "jnz 1b\n\t"
                     "emms\n\t"
                     : [in] "+r"(in), [out] "+r"(out), [n] "+r"(groups)
                     : [c] "r"(c)
                     : "memory", "cc", "mm0", "mm1", "mm2", "mm6", "mm7");
}
#endif

void aMixImpl(uint16_t count, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    ux_u64 t0 = mixer_cycles();
    check_addrs("aMix", in_addr, out_addr, 0);

#if UX_MIXER_MMX
    aMixImplMmx(count, gain, in_addr, out_addr);
#else
    aMixImplRef(count, gain, in_addr, out_addr);
#endif
    sTicks[T_MIX] += mixer_cycles() - t0;
    check_guard("aMix", count, in_addr, out_addr);
}

void aS8DecImpl(uint8_t flags, ADPCM_STATE state) {
    ux_u64 t0 = mixer_cycles();
    uint8_t* in = BUF_U8(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);
        *out++ = (int16_t)(*in++ << 8);

        nbytes -= 16 * sizeof(int16_t);
    }

    memcpy(state, out - 16, 16 * sizeof(int16_t));
    sTicks[T_DECODE] += mixer_cycles() - t0;
    check_guard("aS8Dec", flags, 0, 0);
}

static void aAddMixerRef(uint16_t count, uint16_t in_addr, uint16_t out_addr) {
    int16_t* in = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int nbytes = ROUND_UP_64(ROUND_DOWN_16(count));

    do {
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;
        *out = clamp16(*out + *in++);
        out++;

        nbytes -= 16 * sizeof(int16_t);
    } while (nbytes > 0);
}

#if UX_MIXER_MMX
/* out = clamp16(out + in): paddsw, sixteen samples a pass as the C's */
static void aAddMixerMmx(uint16_t count, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_64(ROUND_DOWN_16(count));
    int16_t* in = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int passes = nbytes > 0 ? nbytes / 32 : 1; /* a do-while: one at least */

    if (in != out && overlap(in, out, passes * 32)) {
        aAddMixerRef(count, in_addr, out_addr);
        return;
    }
    __asm__ volatile("1:\n\t"
                     "movq 0(%[out]), %%mm0\n\t" "paddsw 0(%[in]), %%mm0\n\t" "movq %%mm0, 0(%[out])\n\t"
                     "movq 8(%[out]), %%mm1\n\t" "paddsw 8(%[in]), %%mm1\n\t" "movq %%mm1, 8(%[out])\n\t"
                     "movq 16(%[out]), %%mm0\n\t" "paddsw 16(%[in]), %%mm0\n\t" "movq %%mm0, 16(%[out])\n\t"
                     "movq 24(%[out]), %%mm1\n\t" "paddsw 24(%[in]), %%mm1\n\t" "movq %%mm1, 24(%[out])\n\t"
                     "add $32, %[in]\n\t"
                     "add $32, %[out]\n\t"
                     "dec %[n]\n\t"
                     "jnz 1b\n\t"
                     "emms\n\t"
                     : [in] "+r"(in), [out] "+r"(out), [n] "+r"(passes)
                     :
                     : "memory", "cc", "mm0", "mm1");
}
#endif

void aAddMixerImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr) {
    ux_u64 t0 = mixer_cycles();
    check_addrs("aAddMixer", in_addr, out_addr, 0);

#if UX_MIXER_MMX
    aAddMixerMmx(count, in_addr, out_addr);
#else
    aAddMixerRef(count, in_addr, out_addr);
#endif
    sTicks[T_MIX] += mixer_cycles() - t0;
    check_guard("aAddMixer", count, in_addr, out_addr);
}

void aDuplicateImpl(uint16_t count, uint16_t in_addr, uint16_t out_addr) {
    check_addrs("aDuplicate", in_addr, out_addr, (unsigned)out_addr + (count + 1) * 128 > DMEM_BUF_SIZE ? 0xFFFF : 0);
    uint8_t* in = BUF_U8(in_addr);
    uint8_t* out = BUF_U8(out_addr);

    uint8_t tmp[128];
    memcpy(tmp, in, 128);
    do {
        memcpy(out, tmp, 128);
        out += 128;
    } while (count-- > 0);
    check_guard("aDuplicate", count, in_addr, out_addr);
}

void aResampleZohImpl(uint16_t pitch, uint16_t start_fract) {
    ux_u64 t0 = mixer_cycles();
    int16_t* in = BUF_S16(rspa.in);
    int16_t* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_8(rspa.nbytes);
    uint32_t pos = start_fract;
    uint32_t pitch_add = pitch << 2;

    do {
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;

        nbytes -= 4 * sizeof(int16_t);
    } while (nbytes > 0);
    sTicks[T_RESAMPLE] += mixer_cycles() - t0;
    check_guard("aResampleZoh", pitch, start_fract, 0);
}

void aInterlImpl(uint16_t in_addr, uint16_t out_addr, uint16_t n_samples) {
    check_addrs("aInterl", in_addr, out_addr, (unsigned)out_addr + n_samples * 2 > DMEM_BUF_SIZE ? 0xFFFF : 0);
    int16_t* in = BUF_S16(in_addr);
    int16_t* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);

    do {
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;
        *out++ = *in++;
        in++;

        n -= 8;
    } while (n > 0);
    check_guard("aInterl", in_addr, out_addr, n_samples);
}

static void aFilterRef(uint8_t flags, uint16_t count_or_buf, int16_t* state_or_filter) {
    if (flags > A_INIT) {
        rspa.filter_count = ROUND_UP_16(count_or_buf);
        memcpy(rspa.filter, state_or_filter, sizeof(rspa.filter));
    } else {
        int16_t tmp[16], tmp2[8];
        int count = rspa.filter_count;
        int16_t* buf = BUF_S16(count_or_buf);

        if (flags == A_INIT) {
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmemset-elt-size"
#endif
            memset(tmp, 0, 8 * sizeof(int16_t));
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
            memset(tmp2, 0, 8 * sizeof(int16_t));
        } else {
            memcpy(tmp, state_or_filter, 8 * sizeof(int16_t));
            memcpy(tmp2, state_or_filter + 8, 8 * sizeof(int16_t));
        }

        for (int i = 0; i < 8; i++) {
            rspa.filter[i] = (tmp2[i] + rspa.filter[i]) / 2;
        }

        do {
            memcpy(tmp + 8, buf, 8 * sizeof(int16_t));
            for (int i = 0; i < 8; i++) {
                int64_t sample = 0x4000; // round term
                for (int j = 0; j < 8; j++) {
                    sample += tmp[i + j] * rspa.filter[7 - j];
                }
                buf[i] = clamp16((int32_t)(sample >> 15));
            }
            memcpy(tmp, tmp + 8, 8 * sizeof(int16_t));

            buf += 8;
            count -= 8 * sizeof(int16_t);
        } while (count > 0);

        memcpy(state_or_filter, tmp, 8 * sizeof(int16_t));
        memcpy(state_or_filter + 8, rspa.filter, 8 * sizeof(int16_t));
    }
}

#if UX_MIXER_MMX
/* The filter's eight outputs a pass as dot products of the window tmp[0..15]
   (the last pass's inputs, then this one's, in mm4-mm7) with rows: output i
   is tmp[i + j] * filter[7 - j], so its row holds filter[7 - (k - i)] for k
   from i to i + 7 and zeros elsewhere, and only the quadwords it touches are
   multiplied. The C sums in 64 bits; while the coefficients' magnitudes add
   up to no more than 65535 every sum (the rounding term too) is below 2^31
   and 32 bits give the same (the game's filters reach 45207); past that, the
   C. */
static int16_t sFilterRows[8][16] __attribute__((aligned(8)));
static const uint32_t kFilterRound[2] __attribute__((aligned(8))) = { 0x4000, 0x4000 };

/* output i's sum in d (two dwords to add), its row's quadwords q (window
   register w) at byte offset o */
#define FILT_FIRST(w, o, d) "movq %%" w ", %%" d "\n\t" "pmaddwd " #o "(%[r]), %%" d "\n\t"
#define FILT_NEXT(w, o, d) "movq %%" w ", %%mm1\n\t" "pmaddwd " #o "(%[r]), %%mm1\n\t" "paddd %%mm1, %%" d "\n\t"
/* outputs i and i + 1 (in mm0 and mm2) into mm0's two dwords, rounded, >> 15 */
#define FILT_PAIR_END                                                                                          \
    "movq %%mm0, %%mm1\n\t" "punpckldq %%mm2, %%mm0\n\t" "punpckhdq %%mm2, %%mm1\n\t" "paddd %%mm1, %%mm0\n\t" \
    "paddd %[rnd], %%mm0\n\t" "psrad $15, %%mm0\n\t"

static void aFilterMmx(uint8_t flags, uint16_t count_or_buf, int16_t* state_or_filter) {
    int16_t tmp[8] __attribute__((aligned(8))), tmp2[8];
    int count = rspa.filter_count;
    int16_t* buf = BUF_S16(count_or_buf);
    int passes = count > 0 ? (count + 15) / 16 : 1; /* a do-while: one at least */
    int sum = 0, i, k;

    if (flags > A_INIT) {
        aFilterRef(flags, count_or_buf, state_or_filter);
        return;
    }
    if (flags == A_INIT) {
        memset(tmp, 0, sizeof(tmp));
        memset(tmp2, 0, sizeof(tmp2));
    } else {
        memcpy(tmp, state_or_filter, sizeof(tmp));
        memcpy(tmp2, state_or_filter + 8, sizeof(tmp2));
    }
    for (i = 0; i < 8; i++) {
        int f = (tmp2[i] + rspa.filter[i]) / 2;

        sum += f < 0 ? -f : f;
    }
    if (sum > 65535) {
        aFilterRef(flags, count_or_buf, state_or_filter);
        return;
    }
    for (i = 0; i < 8; i++) {
        rspa.filter[i] = (tmp2[i] + rspa.filter[i]) / 2;
    }
    for (i = 0; i < 8; i++) {
        for (k = 0; k < 16; k++) {
            sFilterRows[i][k] = (k >= i && k - i <= 7) ? rspa.filter[7 - (k - i)] : 0;
        }
    }
    __asm__ volatile("movq 0(%[t]), %%mm4\n\t"
                     "movq 8(%[t]), %%mm5\n\t"
                     "1:\n\t"
                     "movq 0(%[buf]), %%mm6\n\t"
                     "movq 8(%[buf]), %%mm7\n\t"
                     FILT_FIRST("mm4", 0, "mm0") FILT_NEXT("mm5", 8, "mm0")
                     FILT_FIRST("mm4", 32, "mm2") FILT_NEXT("mm5", 40, "mm2") FILT_NEXT("mm6", 48, "mm2")
                     FILT_PAIR_END "movq %%mm0, %%mm3\n\t"
                     FILT_FIRST("mm4", 64, "mm0") FILT_NEXT("mm5", 72, "mm0") FILT_NEXT("mm6", 80, "mm0")
                     FILT_FIRST("mm4", 96, "mm2") FILT_NEXT("mm5", 104, "mm2") FILT_NEXT("mm6", 112, "mm2")
                     FILT_PAIR_END "packssdw %%mm0, %%mm3\n\t"
                     "movq %%mm3, 0(%[buf])\n\t"
                     FILT_FIRST("mm5", 136, "mm0") FILT_NEXT("mm6", 144, "mm0")
                     FILT_FIRST("mm5", 168, "mm2") FILT_NEXT("mm6", 176, "mm2") FILT_NEXT("mm7", 184, "mm2")
                     FILT_PAIR_END "movq %%mm0, %%mm3\n\t"
                     FILT_FIRST("mm5", 200, "mm0") FILT_NEXT("mm6", 208, "mm0") FILT_NEXT("mm7", 216, "mm0")
                     FILT_FIRST("mm5", 232, "mm2") FILT_NEXT("mm6", 240, "mm2") FILT_NEXT("mm7", 248, "mm2")
                     FILT_PAIR_END "packssdw %%mm0, %%mm3\n\t"
                     "movq %%mm3, 8(%[buf])\n\t"
                     "movq %%mm6, %%mm4\n\t"
                     "movq %%mm7, %%mm5\n\t"
                     "add $16, %[buf]\n\t"
                     "dec %[n]\n\t"
                     "jnz 1b\n\t"
                     "movq %%mm4, 0(%[t])\n\t"
                     "movq %%mm5, 8(%[t])\n\t"
                     "emms\n\t"
                     : [buf] "+r"(buf), [n] "+r"(passes)
                     : [t] "r"(tmp), [r] "r"(sFilterRows), [rnd] "m"(kFilterRound)
                     : "memory", "cc", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7");
    memcpy(state_or_filter, tmp, 8 * sizeof(int16_t));
    memcpy(state_or_filter + 8, rspa.filter, 8 * sizeof(int16_t));
}
#endif

void aFilterImpl(uint8_t flags, uint16_t count_or_buf, int16_t* state_or_filter) {
    ux_u64 t0;

    if (flags > A_INIT) {
        aFilterRef(flags, count_or_buf, state_or_filter); /* the count and coefficients */
        return;
    }
    t0 = mixer_cycles();
#if UX_MIXER_MMX
    aFilterMmx(flags, count_or_buf, state_or_filter);
#else
    aFilterRef(flags, count_or_buf, state_or_filter);
#endif
    sTicks[T_FILTER] += mixer_cycles() - t0;
    check_guard("aFilter", flags, count_or_buf, 0);
}

void aHiLoGainImpl(uint8_t g, uint16_t count, uint16_t addr) {
    check_addrs("aHiLoGain", addr, (unsigned)addr + count > DMEM_BUF_SIZE ? 0xFFFF : 0, 0);
    int16_t* samples = BUF_S16(addr);
    int nbytes = ROUND_UP_32(count);

    do {
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;
        *samples = clamp16((*samples * g) >> 4);
        samples++;

        nbytes -= 8;
    } while (nbytes > 0);
    check_guard("aHiLoGain", g, count, addr);
}

void aUnkCmd3Impl(uint16_t a, uint16_t b, uint16_t c) {
}

void aUnkCmd19Impl(uint8_t f, uint16_t count, uint16_t out_addr, uint16_t in_addr) {
    check_addrs("aUnkCmd19", in_addr, out_addr, (unsigned)out_addr + count > DMEM_BUF_SIZE ? 0xFFFF : 0);
    int nbytes = ROUND_UP_64(count);
    int16_t* in = BUF_S16(in_addr + f);
    int16_t* out = BUF_S16(out_addr);
    int16_t tbl[32];

    memcpy(tbl, in, 32 * sizeof(int16_t));
    do {
        for (int i = 0; i < 32; i++) {
            out[i] = clamp16(out[i] * tbl[i]);
        }
        out += 32;
        nbytes -= 32 * sizeof(int16_t);
    } while (nbytes > 0);
    check_guard("aUnkCmd19", f, count, out_addr);
}

