/* lusx_prelude.h -- included first in every Lus-X compile (after the RXDK's
 * picolibc.h): what the RXDK's picolibc + libc++ need so that code written
 * for glibc/MSVC builds unchanged. */
#pragma once

/* glibc and MSVC headers bring malloc/free in with almost anything; fmt (in
   spdlog) relies on that */
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
/* ...and std::strtoull and friends with any standard header */
#include <cstdlib>
#include <cstring>
#endif

/* picolibc's _GNU_SOURCE basename is a function-like macro, which breaks
   every member named basename (spdlog's short_filename_formatter) */
#undef basename

/* The audio as upstream 2Ship has it: every sound font (and its samples) loaded at start, the audio heap's
   pools at full size (~6.5 MB more). 0 is the Xbox's memory-saving lazy fonts and smaller pools
   (src/audio/lib/load.c, include/z64.h) -- with which the audio engine never finishes its heap reset
   (Audio_Update's body never runs: no music, no sound effects; measured, Nov 3). 0 worked on Nov 5, with live notes
   no longer skipped (playback.c) and the audio thread able to do file I/O (OTRAudio_Init), but the fonts it leaves
   out load as they're played and stay: 5.4 MB saved at the start was 1.7 MB after 90 s. Left at 1. */
#define LUSX_AUDIO_AS_UPSTREAM 1
