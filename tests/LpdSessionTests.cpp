#include <LpdSession.h>
#include <ServiceConfiguration.hpp>

#include "PrintSessionFakes.h"
#include "TestSupport.h"

#include <spdlog/spdlog.h>

#include <limits>
#include <optional>
#include <vector>

namespace {

using test_support::JobFactory;
using test_support::JobState;
using test_support::LpdTransport;
constexpr char kAck = 0;
constexpr char kNak = 1;

std::string line(char command, const std::string& arguments) {
    return std::string(1, command) + arguments + "\n";
}

std::string beginJob(const std::string& queue = "Queue") { return line(2, queue); }

std::string fileHeader(char command, const std::string& count, const std::string& name) {
    return line(command, count + " " + name);
}

std::string file(char command, const std::string& name, const std::string& contents, char terminator = 0) {
    return fileHeader(command, std::to_string(contents.size()), name) + contents + terminator;
}

std::optional<std::string> resolve(const std::string& queue) {
    return ResolveLpdQueuePrinter({{"Queue", "Mapped Printer"}}, queue);
}

const JobState& onlyJob(const JobFactory& factory) {
    require(factory.jobs.size() == 1, "Expected exactly one job");
    return *factory.jobs.front();
}

void requireCommitted(const JobState& job, const std::string& data) {
    require(job.commitCalls == 1 && job.committed && job.cancelCalls == 0, "Job must commit exactly once");
    require(job.data == data, "Printed bytes changed");
    require(job.destructionCalls == 1 && !job.abandonedOnDestruction, "Committed job lifecycle");
    require(!job.events.empty() && job.events.back() == "commit", "Commit must be the final job operation");
}

void requireAbandoned(const JobState& job) {
    require(!job.committed && job.commitCalls == 0 && job.cancelCalls == 0, "No explicit commit/cancel on failure");
    require(job.destructionCalls == 1 && job.abandonedOnDestruction,
        "Incomplete LPD jobs are destroyed; production PrinterJob performs destructor cleanup");
}

void testControlFirstAndDataFirst() {
    const std::string data = bytes({0x41, 0, 0xFF, 0x42});
    const std::string control = "Hhost\nPuser\nJBinary document\nNsource\nLbanner\nXignored\n\nfdfA";
    for (const auto chunk : {size_t{1}, size_t{2}, size_t{17}}) {
        for (const bool controlFirst : {false, true}) {
            const auto controlFile = file(2, "cfA", control);
            const auto dataFile = file(3, "dfA", data);
            LpdTransport transport(beginJob("qUeUe") +
                (controlFirst ? controlFile + dataFile : dataFile + controlFile) + "unread", chunk);
            JobFactory factory;
            LpdSession session(factory, resolve, control.size());
            session.Run(transport);
            const auto& job = onlyJob(factory);
            requireCommitted(job, data);
            require(job.printerName == "Mapped Printer" && job.initialDocumentName == "lpr_job" &&
                job.documentName == "Binary document", "Resolved printer and J document name");
            require(transport.responses == std::string(5, kAck), "Exact-count ACK sequence");
            require(transport.readLineCalls == 3 && transport.Remaining() == 6, "Stop reading immediately on completion");
            const auto expectedReads = controlFirst ? std::vector<size_t>{control.size(), 1, data.size(), 1}
                : std::vector<size_t>{data.size(), 1, control.size(), 1};
            require(transport.exactReads == expectedReads && transport.readToEndCalls == 0, "Counted read boundaries");
            // A real LPD client waits for the ACK before sending a file body, so
            // the trace must show ACK before the corresponding read, not just
            // the correct set of final response bytes.
            const std::vector<std::string> expectedEvents = controlFirst
                ? std::vector<std::string>{"readline", "ack", "readline", "ack",
                    "read_exact:" + std::to_string(control.size()), "read_exact:1", "ack",
                    "readline", "ack", "read_exact:" + std::to_string(data.size()), "read_exact:1", "ack"}
                : std::vector<std::string>{"readline", "ack", "readline", "ack",
                    "read_exact:" + std::to_string(data.size()), "read_exact:1", "ack",
                    "readline", "ack", "read_exact:" + std::to_string(control.size()), "read_exact:1", "ack"};
            require(transport.events == expectedEvents, "File-command ACK must precede reading the file body");
        }
    }
}

void testUnknownAndPlaceholderCounts() {
    const std::string control = "fdfA\n";
    const auto prefix = beginJob() + file(2, "cfA", control);
    const std::string data = bytes({0, 0xFE, 0x41, 0, 0x42});
    for (const auto* count : {"0", "99999", "18446744073709551615"}) {
        LpdTransport transport(prefix + fileHeader(3, count, "dfA") + data, 2);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireCommitted(onlyJob(factory), data);
        require(transport.responses == std::string(4, kAck) && transport.Remaining() == 0,
            "EOF compatibility path has no final data ACK");
        const bool readToEnd = std::string(count) == "0" ||
            (std::string(count) == "18446744073709551615" && sizeof(size_t) < sizeof(uint64_t));
        require(transport.readToEndCalls == (readToEnd ? 1U : 0U), "Architecture-dependent count path");
        require(transport.exactReads.size() == (readToEnd ? 2U : 3U), "No terminator read on EOF fallback");
    }
    // With no control file yet, EOF cannot complete an unknown-length job.
    LpdTransport beforeControl(beginJob() + fileHeader(3, "0", "dfA") + data);
    JobFactory factory;
    LpdSession session(factory, resolve);
    session.Run(beforeControl);
    requireAbandoned(onlyJob(factory));
    require(onlyJob(factory).data == data && beforeControl.responses == std::string(2, kAck),
        "Data-first Count=0 cleanup and ACKs");

    LpdTransport emptyFallback(prefix + fileHeader(3, "99999", "dfA"));
    JobFactory emptyFactory;
    LpdSession emptySession(emptyFactory, resolve);
    emptySession.Run(emptyFallback);
    requireAbandoned(onlyJob(emptyFactory));
    require(emptyFallback.responses == std::string(4, kAck) + kNak, "Counted EOF before any data must reject");
}

void testMultipleFilesAndAbortReset() {
    const std::string control = "JMulti\nfdfA\nldfA\nfdfB\n";
    LpdTransport multi(beginJob() + file(2, "cfA", control) + file(3, "dfB", "second") + file(3, "dfA", "first"));
    JobFactory factory;
    LpdSession session(factory, resolve);
    session.Run(multi);
    requireCommitted(onlyJob(factory), "secondfirst");
    require(multi.responses == std::string(7, kAck), "Two data files require seven ACKs");

    // Old control and received-file sets must both be discarded by AbortJob.
    LpdTransport aborted(beginJob() + file(2, "cfOld", "JOld\nfdfA\nfdfB\n") + file(3, "dfA", "old") +
        line(1, "") + file(2, "cfNew", "JNew\nfdfA\n") + file(3, "dfA", "new"));
    JobFactory abortFactory;
    LpdSession abortSession(abortFactory, resolve);
    abortSession.Run(aborted);
    require(abortFactory.jobs.size() == 2 && abortFactory.createCalls == 2, "Abort must create one replacement");
    const auto& old = *abortFactory.jobs[0];
    require(old.cancelCalls == 1 && old.commitCalls == 0 && !old.committed && old.data == "old" &&
        old.destructionCalls == 1 && !old.abandonedOnDestruction, "Old job explicitly canceled before replacement");
    requireCommitted(*abortFactory.jobs[1], "new");
    require(abortFactory.jobs[1]->initialDocumentName == "lpr_job" && abortFactory.jobs[1]->documentName == "New",
        "Replacement document state");
    require(aborted.responses == std::string(10, kAck), "Abort and replacement ACK sequence");
}

void testControlLimitsAndTerminators() {
    JobFactory zeroFactory;
    requireThrows([&] { LpdSession invalid(zeroFactory, resolve, 0); }, "Zero control limit accepted");
    for (const auto* count : {"5", "18446744073709551615"}) {
        LpdTransport transport(beginJob() + fileHeader(2, count, "cfA") + "body must remain");
        JobFactory factory;
        LpdSession session(factory, resolve, 4);
        session.Run(transport);
        requireAbandoned(onlyJob(factory));
        require(transport.responses == std::string{kAck, kNak} && transport.exactReads.empty() &&
            transport.readToEndCalls == 0 && transport.payloadBytesRead == 0 && transport.Remaining() == 16,
            "Oversized control must NAK without reading or allocating payload");
    }
    for (const bool badControl : {false, true}) {
        const auto control = file(2, "cfA", "fdfA\n", badControl ? 'x' : '\0');
        const auto input = beginJob() + control + (badControl ? "" : file(3, "dfA", "abc", 'x'));
        LpdTransport transport(input);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireAbandoned(onlyJob(factory));
        require(transport.responses == std::string(badControl ? 2 : 4, kAck) + kNak, "Bad terminator ACK/NAK sequence");
    }
    // A complete counted payload still needs a NUL terminator; EOF is not one.
    LpdTransport missing(beginJob() + file(2, "cfA", "fdfA\n") + fileHeader(3, "3", "dfA") + "abc");
    JobFactory missingFactory;
    LpdSession missingSession(missingFactory, resolve);
    missingSession.Run(missing);
    requireAbandoned(onlyJob(missingFactory));
    require(missing.responses == std::string(4, kAck) + kNak, "Missing data terminator must reject");
}

void testInvalidCommandsAndMappings() {
    for (const auto& command : std::vector<std::string>{"\n", line(2, ""), line(3, "Queue"),
        line(4, "Queue"), line(5, "Queue"), line(9, "Queue")}) {
        LpdTransport transport(command);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        require(factory.jobs.empty() && transport.responses == std::string(1, kNak), "Invalid top-level command");
    }
    for (const auto* queue : {"Unknown", "Mapped Printer", "Queue "}) {
        LpdTransport transport(beginJob(queue));
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        require(factory.jobs.empty() && transport.responses == std::string(1, kNak), "Unmapped queue must not create a job");
    }
    LpdTransport waiting(line(1, "Unknown"));
    JobFactory waitingFactory;
    LpdSession waitingSession(waitingFactory, resolve);
    waitingSession.Run(waiting);
    require(waitingFactory.jobs.empty() && waiting.responses == std::string(1, kAck),
        "PrintWaitingJobs is acknowledged without resolving the queue");

    const std::vector<std::string> invalid{
        "\n", line(3, "1"), line(3, "1 "), line(3, "x dfA"), line(3, "1x dfA"),
        line(3, " dfA"), line(3, "18446744073709551616 dfA"), line(9, "1 dfA")
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        LpdTransport transport(beginJob() + invalid[index] + "unread");
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireAbandoned(onlyJob(factory));
        require(transport.responses == std::string{kAck, kNak} && transport.exactReads.empty(),
            "Invalid subcommand case " + std::to_string(index));
    }
}

void testFileSetValidation() {
    struct Example {
        std::string input;
        size_t acknowledgments;
        size_t exactReads;
    };
    const std::vector<Example> examples{
        {file(2, "cfA", "JNo references\n"), 2, 2},
        {file(2, "cfA", ""), 2, 2},
        {file(2, "cfA", "fdfA\n") + fileHeader(3, "1", "dfB") + "x", 3, 2},
        {file(3, "dfA", "a") + file(2, "cfA", "fdfB\n"), 4, 4},
        {file(2, "cfA", "fdfA\nfdfB\n") + file(3, "dfA", "a") + fileHeader(3, "1", "dfA"), 5, 4},
        {file(2, "cfA", "fdfA\n") + fileHeader(2, "5", "cfB") + "fdfA\n", 3, 2}
    };
    for (size_t index = 0; index < examples.size(); ++index) {
        const auto& example = examples[index];
        LpdTransport transport(beginJob() + example.input);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireAbandoned(onlyJob(factory));
        require(transport.responses == std::string(example.acknowledgments, kAck) + kNak &&
            transport.exactReads.size() == example.exactReads, "File set rejection case " + std::to_string(index));
    }
}

void testDisconnectAndBackendFailures() {
    {
        LpdTransport disconnected("");
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(disconnected);
        require(factory.jobs.empty() && disconnected.responses.empty(), "Disconnect before initial command");
    }
    for (const auto& suffix : std::vector<std::string>{"", file(3, "dfA", "partial"),
        file(2, "cfA", "fdfA\n")}) {
        LpdTransport disconnected(beginJob() + suffix);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(disconnected);
        requireAbandoned(onlyJob(factory));
        require(disconnected.responses == std::string(suffix.empty() ? 1 : 3, kAck),
            "ReadLine disconnect performs cleanup without an extra NAK");
    }
    {
        LpdTransport transport(beginJob());
        JobFactory factory;
        factory.failCreateAt = 0;
        LpdSession session(factory, resolve);
        session.Run(transport);
        require(factory.jobs.empty() && factory.createCalls == 1 && transport.responses == std::string(1, kNak),
            "Initial Create failure must NAK");
    }
    {
        LpdTransport transport(beginJob() + line(1, ""));
        JobFactory factory;
        factory.failCreateAt = 1;
        LpdSession session(factory, resolve);
        session.Run(transport);
        const auto& job = onlyJob(factory);
        require(job.cancelCalls == 1 && job.destructionCalls == 1 && !job.abandonedOnDestruction &&
            transport.responses == std::string{kAck, kNak}, "Abort replacement Create failure");
    }
    {
        LpdTransport transport(beginJob() + line(1, ""));
        JobFactory factory;
        factory.failCancel = true;
        LpdSession session(factory, resolve);
        session.Run(transport);
        const auto& job = onlyJob(factory);
        require(factory.createCalls == 1 && job.cancelCalls == 1 && job.destructionCalls == 1 &&
            job.abandonedOnDestruction && transport.responses == std::string{kAck, kNak}, "Abort Cancel failure");
    }
    {
        LpdTransport transport(beginJob() + file(2, "cfA", "fdfA\n") + file(3, "dfA", "abc"));
        JobFactory factory;
        factory.failWrite = true;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireAbandoned(onlyJob(factory));
        require(transport.responses == std::string(4, kAck) + kNak, "Write failure must NAK and release job");
    }
}

void testInvalidCountsRejected() {
    const std::string control = "fdfA\n";
    const auto prefix = beginJob() + file(2, "cfA", control);
    const std::string unread("x\0unread", 8);
    // The first space separates count and name, so a trailing space cannot be
    // part of the count here; a leading space in the file name is covered by
    // the file-set validation tests instead.
    const std::vector<std::string> invalid{
        "", "-1", "+1", " 1", "\t1", "1\t", "--1", "1x",
        "18446744073709551616", bytes({0xB2}), std::string("1\0", 2)
    };
    for (size_t index = 0; index < invalid.size(); ++index) {
        LpdTransport transport(prefix + fileHeader(3, invalid[index], "dfA") + unread);
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        const auto& job = onlyJob(factory);
        requireAbandoned(job);
        require(job.data.empty(), "Rejected count must not write any data to the job");
        require(transport.responses == std::string{kAck, kAck, kAck, kNak},
            "Invalid count ACK/NAK sequence, case " + std::to_string(index));
        require(transport.exactReads == std::vector<size_t>{control.size(), 1} &&
            transport.readToEndCalls == 0 && transport.payloadBytesRead == control.size() + 1 &&
            transport.readLineCalls == 3 && transport.Remaining() == unread.size(),
            "Rejected file body must remain unread, case " + std::to_string(index));
    }
}

void testLeadingZeroCounts() {
    for (const auto* count : {"01", "0001"}) {
        LpdTransport transport(beginJob() + file(2, "cfA", "fdfA\n") +
            fileHeader(3, count, "dfA") + std::string("x\0", 2));
        JobFactory factory;
        LpdSession session(factory, resolve);
        session.Run(transport);
        requireCommitted(onlyJob(factory), "x");
        require(transport.responses == std::string(5, kAck) && transport.Remaining() == 0,
            "Leading zero count must retain exact-count ACK sequence");
        require(transport.exactReads == std::vector<size_t>{5, 1, 1, 1} && transport.readToEndCalls == 0,
            "Leading zero count must read one data byte and its terminator");
    }
}

} // namespace

int main() {
    try {
        spdlog::set_level(spdlog::level::off);
        runTest("control/data order and chunking", testControlFirstAndDataFirst);
        runTest("unknown and placeholder counts", testUnknownAndPlaceholderCounts);
        runTest("multiple files and AbortJob", testMultipleFilesAndAbortReset);
        runTest("control limits and terminators", testControlLimitsAndTerminators);
        runTest("invalid commands and queue mappings", testInvalidCommandsAndMappings);
        runTest("file-set validation", testFileSetValidation);
        runTest("disconnect and backend failures", testDisconnectAndBackendFailures);
        runTest("invalid counts rejected", testInvalidCountsRejected);
        runTest("leading zero counts", testLeadingZeroCounts);
    } catch (const std::exception& error) {
        std::cerr << "LPD session tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
