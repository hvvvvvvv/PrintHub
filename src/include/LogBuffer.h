#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/sinks/base_sink.h>

class LogBuffer final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit LogBuffer(std::size_t capacity = 80);
    // Returns up to maxLines formatted lines in chronological order (oldest first).
    std::vector<std::string> Snapshot(std::size_t maxLines);

protected:
    void sink_it_(const spdlog::details::log_msg& message) override;
    void flush_() override;

private:
    std::size_t capacity_;
    std::deque<std::string> lines_;
};
