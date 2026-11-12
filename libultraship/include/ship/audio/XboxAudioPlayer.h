#pragma once

#ifdef LUS_XBOX

#include "AudioPlayer.h"

namespace Ship {
/**
 * @brief Audio output on the original Xbox: a DirectSound stream (the MCPX APU).
 *
 * Play() copies into a ring; a thread of the player's own keeps a few packets
 * queued on the stream, refilled from the ring, silence only when the ring has
 * nothing (the game loading). Buffered() is the ring plus what is queued on the
 * stream, which is what LUS games pace their audio thread by. Stereo s16 only:
 * the Xbox's own encoder makes Dolby Digital from a stereo stream as set in
 * the dashboard.
 */
class XboxAudioPlayer : public AudioPlayer {
  public:
    XboxAudioPlayer(AudioSettings settings) : AudioPlayer(settings) {
    }
    ~XboxAudioPlayer();

    int32_t Buffered() override;

  protected:
    bool DoInit() override;
    void DoClose() override;
    void DoPlay(const uint8_t* buf, size_t len) override;
};
} // namespace Ship

#endif
