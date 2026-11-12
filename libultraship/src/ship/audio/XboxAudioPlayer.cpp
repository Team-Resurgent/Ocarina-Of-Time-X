// XboxAudioPlayer: LUS's audio output on a DirectSound stream (after UltraX's
// ux_xbox_audio.c, which streams the N64's AI the same way).
#ifdef LUS_XBOX

#include "ship/audio/XboxAudioPlayer.h"

#include <xtl.h>
#include <dsound.h>
#include <string.h>
#include <spdlog/spdlog.h>

namespace Ship {

namespace {
constexpr uint32_t kPacket = 512;          // stereo frames a packet plays (~12 ms at 44.1 kHz)
constexpr int kPackets = 4;                // queued on the stream
constexpr uint32_t kRing = kPacket * 16;   // frames the ring holds, a power of two

LPDIRECTSOUND sDS;
LPDIRECTSOUNDSTREAM sStream;
short* sPcm[kPackets];
uint32_t sReal[kPackets]; // a packet's frames of audio (the rest of it silence)
DWORD sStatus[kPackets];
HANDLE sDone[kPackets];
CRITICAL_SECTION sLock;
uint32_t sRing[kRing]; // a frame: left and right s16
volatile uint32_t sHead, sTail;
volatile LONG sQueued;
volatile LONG sQueuedReal; // the queued packets' frames of audio: what Buffered counts, not their silence
volatile bool sRun;
HANDLE sThread;

uint32_t Waiting() {
    return sHead - sTail;
}

// a packet from the ring, the rest silence; the frames of audio in it
uint32_t Render(short* out) {
    uint32_t have, i;

    EnterCriticalSection(&sLock);
    have = Waiting();
    if (have > kPacket) {
        have = kPacket;
    }
    for (i = 0; i < have; i++) {
        reinterpret_cast<uint32_t*>(out)[i] = sRing[sTail & (kRing - 1)];
        sTail++;
    }
    LeaveCriticalSection(&sLock);
    memset(out + have * 2, 0, (kPacket - have) * 4);
    return have;
}

void Submit(int k) {
    XMEDIAPACKET xp;

    memset(&xp, 0, sizeof(xp));
    xp.pvBuffer = sPcm[k];
    xp.dwMaxSize = kPacket * 4;
    xp.pdwStatus = &sStatus[k];
    xp.hCompletionEvent = sDone[k];
    sStatus[k] = XMEDIAPACKET_STATUS_PENDING;
    IDirectSoundStream_Process(sStream, &xp, NULL);
}

DWORD WINAPI StreamThread(LPVOID) {
    int head = 0, queued = kPackets;
    uint32_t packets = 0, short_ = 0; // for the log: packets the game's audio didn't fill (a gap heard)

    while (sRun) {
        // packets complete in order: wait for the oldest (a packet is 16 ms, four are queued; the
        // completion event wakes this at once, the timeout is only a backstop)
        WaitForSingleObject(sDone[head], 8);
        DirectSoundDoWork();
        while (queued > 0 && sStatus[head] != XMEDIAPACKET_STATUS_PENDING) {
            InterlockedExchangeAdd(&sQueuedReal, -(LONG)sReal[head]);
            head = (head + 1) % kPackets;
            queued--;
        }
        while (queued < kPackets && (Waiting() >= kPacket || queued <= 1)) {
            int k = (head + queued) % kPackets;
            sReal[k] = Render(sPcm[k]);
            InterlockedExchangeAdd(&sQueuedReal, (LONG)sReal[k]);
            Submit(k);
            queued++;
            short_ += sReal[k] < kPacket;
            if (++packets == 320) { // ~5 s
                if (short_ > 0) {
                    SPDLOG_INFO("XboxAudioPlayer: {} of the last {} packets short of audio (gaps)", short_, packets);
                }
                packets = short_ = 0;
            }
        }
        InterlockedExchange(&sQueued, queued);
    }
    return 0;
}
} // namespace

XboxAudioPlayer::~XboxAudioPlayer() {
    DoClose();
}

bool XboxAudioPlayer::DoInit() {
    WAVEFORMATEX wfx;
    DSSTREAMDESC desc;

    if (sStream != NULL) {
        return true;
    }
    InitializeCriticalSection(&sLock);
    if (sDS == NULL && FAILED(DirectSoundCreate(NULL, &sDS, NULL))) {
        SPDLOG_ERROR("XboxAudioPlayer: no DirectSound");
        return false;
    }
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = GetSampleRate();
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 4;
    memset(&desc, 0, sizeof(desc));
    desc.dwMaxAttachedPackets = kPackets;
    desc.lpwfxFormat = &wfx;
    if (FAILED(DirectSoundCreateStream(&desc, &sStream))) {
        sStream = NULL;
        SPDLOG_ERROR("XboxAudioPlayer: no DirectSound stream");
        return false;
    }
    for (int k = 0; k < kPackets; k++) {
        // the APU reads packets from physical memory
        sPcm[k] = static_cast<short*>(XPhysicalAlloc(kPacket * 4, MAXULONG_PTR, 0, PAGE_READWRITE));
        sDone[k] = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (sPcm[k] == NULL || sDone[k] == NULL) {
            return false;
        }
        memset(sPcm[k], 0, kPacket * 4);
    }
    for (int k = 0; k < kPackets; k++) {
        Submit(k);
    }
    sQueued = kPackets;
    sRun = true;
    sThread = CreateThread(NULL, 16 * 1024, StreamThread, NULL, 0, NULL);
    if (sThread == NULL) {
        return false;
    }
    SetThreadPriority(sThread, THREAD_PRIORITY_HIGHEST);
    SPDLOG_INFO("XboxAudioPlayer: DirectSound stream, stereo, {} Hz", GetSampleRate());
    return true;
}

void XboxAudioPlayer::DoClose() {
    // the stream stays up for the title's life: the Xbox has the one output
    // and a game only ever closes it to change channels, which it does not
}

// the frames of audio not yet played: the ring's and the queued packets' (their padding of silence not:
// counted as audio, the game was told the stream was full when it had run dry)
int32_t XboxAudioPlayer::Buffered() {
    return static_cast<int32_t>(Waiting() + static_cast<uint32_t>(sQueuedReal));
}

void XboxAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    const uint32_t* in = reinterpret_cast<const uint32_t*>(buf);
    uint32_t n = static_cast<uint32_t>(len / 4);

    if (sStream == NULL) {
        return;
    }
    EnterCriticalSection(&sLock);
    for (uint32_t i = 0; i < n; i++) {
        if (sHead - sTail >= kRing) {
            sTail++; // full: the oldest frame goes
        }
        sRing[sHead & (kRing - 1)] = in[i];
        sHead++;
    }
    LeaveCriticalSection(&sLock);
}
} // namespace Ship

#endif
