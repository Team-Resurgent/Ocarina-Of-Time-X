/* ux_mixer.h -- UltraX: the EAD audio microcode's commands as functions,
 * called as the game's synthesis builds them (2Ship2Harkinian's mixer).
 *
 * An EAD-driver game (Ocarina of Time, Majora's Mask) builds its sound as a
 * list of audio microcode commands (PR/abi.h's aADPCMdec, aEnvMixer...)
 * and has the RSP run it. Included after the game's own headers in its
 * synthesis (audio/lib/synthesis.c), this header turns each of those macros
 * into a call that does the work at once, on the CPU: the sound is mixed
 * into the AI buffer while the synthesis runs, and the list the RSP gets is
 * empty (an audio task with nothing in it). This is how 2Ship2Harkinian
 * plays Majora's Mask on libultraship; ux_mixer.c is its mixer.c (CC0),
 * the portable implementations, the arguments as its macros pass them.
 *
 * What the microcode keeps between commands (DMEM, the ADPCM codebook, the
 * envelope's volumes and rates) is the mixer's own state: only the game's
 * audio thread may call these.
 */
#ifndef ULTRAX_UX_MIXER_H
#define ULTRAX_UX_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif

/* fixed-size integers of its own: a decomp's libc (which the game's
   synthesis is compiled with) has no <stdint.h> types */
typedef unsigned char uxm_u8;
typedef unsigned short uxm_u16;
typedef short uxm_s16;
typedef int uxm_s32;
typedef unsigned int uxm_u32;

void aClearBufferImpl(uxm_u16 addr, int nbytes);
void aLoadBufferImpl(const void* source_addr, uxm_u16 dest_addr, uxm_u16 nbytes);
void aSaveBufferImpl(uxm_u16 source_addr, uxm_s16* dest_addr, uxm_u16 nbytes);
void aLoadADPCMImpl(int num_entries_times_16, const uxm_s16* book_source_addr);
void aSetBufferImpl(uxm_u8 flags, uxm_u16 in, uxm_u16 out, uxm_u16 nbytes);
void aInterleaveImpl(uxm_u16 dest, uxm_u16 left, uxm_u16 right, uxm_u16 c);
void aDMEMMoveImpl(uxm_u16 in_addr, uxm_u16 out_addr, int nbytes);
void aSetLoopImpl(uxm_s16* adpcm_loop_state);
void aADPCMdecImpl(uxm_u8 flags, uxm_s16* state);
void aResampleImpl(uxm_u8 flags, uxm_u16 pitch, uxm_s16* state);
void aEnvSetup1Impl(uxm_u8 initial_vol_wet, uxm_u16 rate_wet, uxm_u16 rate_left, uxm_u16 rate_right);
void aEnvSetup2Impl(uxm_u16 initial_vol_left, uxm_u16 initial_vol_right);
void aEnvMixerImpl(uxm_u16 in_addr, uxm_u16 n_samples, _Bool swap_reverb, _Bool neg_3, _Bool neg_2, _Bool neg_left,
                   _Bool neg_right, uxm_s32 wet_dry_addr, uxm_u32 unk);
void aMixImpl(uxm_u16 count, uxm_s16 gain, uxm_u16 in_addr, uxm_u16 out_addr);
void aS8DecImpl(uxm_u8 flags, uxm_s16* state);
void aAddMixerImpl(uxm_u16 count, uxm_u16 in_addr, uxm_u16 out_addr);
void aDuplicateImpl(uxm_u16 count, uxm_u16 in_addr, uxm_u16 out_addr);
void aResampleZohImpl(uxm_u16 pitch, uxm_u16 start_fract);
void aInterlImpl(uxm_u16 in_addr, uxm_u16 out_addr, uxm_u16 n_samples);
void aFilterImpl(uxm_u8 flags, uxm_u16 count_or_buf, uxm_s16* state_or_filter);
void aHiLoGainImpl(uxm_u8 g, uxm_u16 count, uxm_u16 addr);
void aUnkCmd3Impl(uxm_u16 a, uxm_u16 b, uxm_u16 c);
void aUnkCmd19Impl(uxm_u8 f, uxm_u16 count, uxm_u16 out_addr, uxm_u16 in_addr);

/* To the log: what the mixer did since the last call (ADPCM frames decoded
   at 4 and 2 bits a sample, voices mixed). */
void ux_mixer_log_stats(void);

#ifdef __cplusplus
}
#endif

/* The game's command macros, as calls (the command list pointer goes) */
#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aResample
#undef aInterleave
#undef aSetVolume
#undef aSetVolume32
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aS8Dec
#undef aAddMixer
#undef aDuplicate
#undef aDMEMMove2
#undef aResampleZoh
#undef aEnvSetup1
#undef aEnvSetup2
#undef aFilter
#undef aHiLoGain
#undef aInterl
#undef aUnkCmd3
#undef aUnkCmd19

#define aSegment(pkt, s, b) \
    do {                    \
    } while (0)
#define aClearBuffer(pkt, d, c) aClearBufferImpl(d, c)
#define aLoadBuffer(pkt, s, d, c) aLoadBufferImpl((const void*)(unsigned long)(s), d, c)
#define aSaveBuffer(pkt, s, d, c) aSaveBufferImpl(s, (uxm_s16*)(unsigned long)(d), c)
#define aLoadADPCM(pkt, c, d) aLoadADPCMImpl(c, (const uxm_s16*)(unsigned long)(d))
#define aSetBuffer(pkt, f, i, o, c) aSetBufferImpl(f, i, o, c)
#define aInterleave(pkt, o, l, r, c) aInterleaveImpl(o, l, r, c)
#define aDMEMMove(pkt, i, o, c) aDMEMMoveImpl(i, o, c)
#define aSetLoop(pkt, a) aSetLoopImpl((uxm_s16*)(a))
#define aADPCMdec(pkt, f, s) aADPCMdecImpl(f, (uxm_s16*)(s))
#define aResample(pkt, f, p, s) aResampleImpl(f, p, (uxm_s16*)(s))
#define aEnvSetup1(pkt, initialVolReverb, rampReverb, rampLeft, rampRight) \
    aEnvSetup1Impl(initialVolReverb, rampReverb, rampLeft, rampRight)
#define aEnvSetup2(pkt, initialVolLeft, initialVolRight) aEnvSetup2Impl(initialVolLeft, initialVolRight)
#define aEnvMixer(pkt, inBuf, nSamples, swapReverb, negLeft, negRight, dryLeft, dryRight, wetLeft, wetRight) \
    aEnvMixerImpl(inBuf, nSamples, swapReverb, negLeft, negRight, dryLeft, dryRight, wetLeft, wetRight)
#define aMix(pkt, c, g, i, o) aMixImpl(c, g, i, o)
#define aS8Dec(pkt, f, s) aS8DecImpl(f, (uxm_s16*)(s))
/* the microcode's ADDMIXER has a fifth argument the game passes as 0x7FFF; the mixer has none */
#define aAddMixer(pkt, s, d, c, a4) aAddMixerImpl(s, d, c)
#define aDuplicate(pkt, s, d, c) aDuplicateImpl(s, d, c)
#define aResampleZoh(pkt, pitch, startFract) aResampleZohImpl(pitch, startFract)
#define aInterl(pkt, dmemi, dmemo, count) aInterlImpl(dmemi, dmemo, count)
#define aFilter(pkt, f, countOrBuf, addr) aFilterImpl(f, countOrBuf, (uxm_s16*)(unsigned long)(addr))
#define aHiLoGain(pkt, g, buflen, i, a4) aHiLoGainImpl(g, buflen, i)
#define aUnkCmd3(pkt, a1, a2, a3) aUnkCmd3Impl(a1, a2, a3)
#define aUnkCmd19(pkt, a1, a2, a3, a4) aUnkCmd19Impl(a1, a2, a3, a4)

#endif
