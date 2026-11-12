#pragma once
#ifdef LUS_XBOX

#include <mutex>
#include <spdlog/sinks/base_sink.h>

#include "lusx_log.h"

namespace Ship {
/**
 * @brief spdlog to the Xbox's debug channel (what xbWatson and the debugger show), by way of the port menu's Logging
 * switch (xbox/include/lusx_log.h).
 */
class XboxDebugSink final : public spdlog::sinks::base_sink<std::mutex> {
  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        formatted.push_back('\0');
        LusxLog_Send(formatted.data());
    }

    void flush_() override {
    }
};
} // namespace Ship

#endif
