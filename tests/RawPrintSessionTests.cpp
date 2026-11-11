#include <RawPrintSession.h>

#include "PrintSessionFakes.h"
#include "TestSupport.h"

#include <vector>

namespace {

using test_support::JobFactory;
using test_support::RawTransport;

void testDataAndRepeatedRuns() {
    JobFactory factory;
    RawPrintSession session(factory, "RAW Printer");
    const std::vector<std::vector<std::string>> inputs{
        {"hello", " ", "world"}, {bytes({0, 0xFF}), "", bytes({0x41, 0, 0x80})},
        {}, {""}, {"final"}
    };
    const std::vector<std::string> expected{"hello world", bytes({0, 0xFF, 0x41, 0, 0x80}), "", "", "final"};
    for (size_t index = 0; index < inputs.size(); ++index) {
        RawTransport transport(inputs[index]);
        session.Run(transport);
        require(transport.reads == 1 && factory.jobs.size() == index + 1, "Run must create/read one independent job");
        const auto& job = *factory.jobs[index];
        require(job.printerName == "RAW Printer" && job.initialDocumentName == "raw_job" &&
            job.documentName == "raw_job", "RAW printer and default document");
        require(job.data == expected[index], "Binary data and chunk boundaries");
        const bool empty = expected[index].empty();
        require(job.commitCalls == (empty ? 0U : 1U) && job.cancelCalls == (empty ? 1U : 0U) &&
            job.committed == !empty, "RAW commit versus explicit empty-job cancel");
        require(job.destructionCalls == 1 && !job.abandonedOnDestruction, "RAW lifecycle after Run");
        require(!job.events.empty() && job.events.back() == (empty ? "cancel" : "commit"), "Terminal job operation");
        for (size_t earlier = 0; earlier < index; ++earlier) {
            require(factory.jobs[earlier]->data == expected[earlier] &&
                factory.jobs[earlier]->destructionCalls == 1, "Later Run modified an earlier job");
        }
    }
}

void testFailuresReleaseOwnership() {
    JobFactory invalidFactory;
    requireThrows([&] { RawPrintSession invalid(invalidFactory, ""); }, "Empty RAW printer accepted");
    {
        JobFactory factory;
        factory.failCreateAt = 0;
        RawPrintSession session(factory, "P");
        RawTransport transport({"x"});
        requireThrows<std::runtime_error>([&] { session.Run(transport); }, "Create failure must propagate");
        require(factory.jobs.empty() && transport.reads == 0, "Failed Create must not consume input");
    }
    for (const bool writeFailure : {false, true}) {
        JobFactory factory;
        factory.failWrite = writeFailure;
        RawPrintSession session(factory, "P");
        RawTransport transport({"data"});
        transport.failAfterRead = !writeFailure;
        requireThrows<std::runtime_error>([&] { session.Run(transport); }, "Read/write failure must propagate");
        require(factory.jobs.size() == 1, "Failed RAW job created");
        const auto& job = *factory.jobs.front();
        require(job.commitCalls == 0 && job.cancelCalls == 0 && job.destructionCalls == 1 &&
            job.abandonedOnDestruction, "Exception must release job without pretending to call Cancel");
    }
    {
        JobFactory factory;
        factory.failCommit = true;
        RawPrintSession session(factory, "P");
        RawTransport transport({"data"});
        requireThrows<std::runtime_error>([&] { session.Run(transport); }, "Commit failure must propagate");
        const auto& job = *factory.jobs.front();
        require(job.commitCalls == 1 && !job.committed && job.destructionCalls == 1 &&
            job.abandonedOnDestruction, "Commit failure lifecycle");
    }
}

} // namespace

int main() {
    try {
        runTest("data, binary bytes and independent repeated runs", testDataAndRepeatedRuns);
        runTest("failure ownership", testFailuresReleaseOwnership);
    } catch (const std::exception& error) {
        std::cerr << "RAW session tests failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
