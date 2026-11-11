#include <LogBuffer.h>

#include "TestSupport.h"

#include <memory>
#include <regex>
#include <thread>
#include <type_traits>
#include <vector>

#include <spdlog/details/os.h>
#include <spdlog/logger.h>

namespace {

static_assert(!std::is_copy_constructible_v<LogBuffer> && !std::is_copy_assignable_v<LogBuffer>);
static_assert(!std::is_move_constructible_v<LogBuffer> && !std::is_move_assignable_v<LogBuffer>);

void testCapacityAndSnapshots() {
    auto sink = std::make_shared<LogBuffer>(80);
    auto logger = std::make_shared<spdlog::logger>("buffer-test", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");
    require(sink->Snapshot(80).empty(), "New buffer must be empty");
    for (int line = 0; line < 100; ++line) logger->info("line {}", line);

    // Keep the complete formatter output, including spdlog's platform line ending.
    const auto lines = sink->Snapshot(80);
    require(lines.size() == 80, "Buffer must retain exactly 80 entries");
    for (std::size_t index = 0; index < lines.size(); ++index) {
        require(lines[index] == "line " + std::to_string(index + 20) + spdlog::details::os::default_eol,
            "Buffer must retain lines 20..99 in chronological order");
    }
    const auto lastTen = sink->Snapshot(10);
    require(lastTen.size() == 10, "Snapshot must respect its requested size");
    for (std::size_t index = 0; index < lastTen.size(); ++index) {
        require(lastTen[index] == "line " + std::to_string(index + 90) + spdlog::details::os::default_eol,
            "Small snapshot must return lines 90..99");
    }
    require(sink->Snapshot(200) == lines, "Oversized snapshot must return only retained entries");
    require(sink->Snapshot(0).empty(), "Zero-length snapshot must be empty");
    logger->flush();
    require(sink->Snapshot(80) == lines, "Snapshots and flush must not consume buffered logs");
    logger->info("line 100");
    require(lines.back() == std::string("line 99") + spdlog::details::os::default_eol,
        "Previously returned snapshots must own their contents");
    require(sink->Snapshot(1) == std::vector<std::string>{
        std::string("line 100") + spdlog::details::os::default_eol}, "Buffer must continue accepting logs");
}

void testPatternAndFrontendPrefix() {
    auto sink = std::make_shared<LogBuffer>();
    auto logger = std::make_shared<spdlog::logger>("pattern-test", sink);
    logger->set_pattern("%v");
    logger->info("before pattern change");
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %l : %v");
    logger->info("formatted message");

    const auto lines = sink->Snapshot(80);
    require(lines.size() == 2, "Pattern change must preserve buffered entries");
    require(lines.front() == std::string("before pattern change") + spdlog::details::os::default_eol,
        "Pattern change must not reformat earlier entries");
    // Match the existing frontend's timestamp and level-field expression.
    const std::regex levelField(
        R"(^\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:[.,]\d+)?\s+([a-z]+)\s*:)",
        std::regex::icase);
    std::smatch match;
    require(std::regex_search(lines.back(), match, levelField), "Formatted line must match the frontend");
    require(match[1].str() == "info", "Frontend must extract the info level");
    require(lines.back().ends_with(std::string(" info : formatted message") + spdlog::details::os::default_eol),
        "Sink must use the logger's updated formatter");
}

void testConcurrentWriters() {
    auto sink = std::make_shared<LogBuffer>();
    auto logger = std::make_shared<spdlog::logger>("concurrent-test", sink);
    logger->set_pattern("%v");
    {
        std::vector<std::jthread> writers;
        for (int writer = 0; writer < 4; ++writer) {
            writers.emplace_back([logger, writer] {
                for (int line = 0; line < 50; ++line) logger->info("thread {} line {}", writer, line);
            });
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            require(sink->Snapshot(80).size() <= 80, "Concurrent snapshot exceeded buffer capacity");
            std::this_thread::yield();
        }
    } // Join all writers before checking the final size, without assuming inter-thread order.
    require(sink->Snapshot(80).size() == 80, "Concurrent writes must retain the default 80 entries");
}

void testZeroCapacity() {
    auto sink = std::make_shared<LogBuffer>(0);
    auto logger = std::make_shared<spdlog::logger>("zero-capacity-test", sink);
    logger->set_pattern("%v");
    logger->info("discarded");
    logger->info("retained");
    require(sink->Snapshot(80) == std::vector<std::string>{
        std::string("retained") + spdlog::details::os::default_eol}, "Zero capacity must behave as capacity one");
}

} // namespace

int main() {
    try {
        runTest("capacity and snapshots", testCapacityAndSnapshots);
        runTest("pattern and frontend prefix", testPatternAndFrontendPrefix);
        runTest("concurrent writers", testConcurrentWriters);
        runTest("zero capacity", testZeroCapacity);
    } catch (const std::exception& error) {
        std::cerr << "Log buffer tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
