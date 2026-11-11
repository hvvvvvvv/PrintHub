#include <LogBuffer.h>

#include <algorithm>
#include <spdlog/formatter.h>

LogBuffer::LogBuffer(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

std::vector<std::string> LogBuffer::Snapshot(std::size_t maxLines) {
    std::lock_guard lock(mutex_);
    const auto count = (std::min)(maxLines, lines_.size());
    return std::vector<std::string>(lines_.end() - static_cast<std::ptrdiff_t>(count), lines_.end());
}

void LogBuffer::sink_it_(const spdlog::details::log_msg& message) {
    // base_sink::log already holds mutex_, also protecting formatter_.
    spdlog::memory_buf_t formatted;
    formatter_->format(message, formatted);
    lines_.emplace_back(formatted.data(), formatted.size());
    if (lines_.size() > capacity_) lines_.pop_front();
}

void LogBuffer::flush_() {}
