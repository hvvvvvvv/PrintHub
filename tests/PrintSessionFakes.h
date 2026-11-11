#pragma once

#include <LpdSession.h>
#include <PrintJob.h>
#include <RawPrintSession.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_support {

struct JobState {
    std::string printerName;
    std::string initialDocumentName;
    std::string documentName;
    std::string data;
    std::vector<std::string> events;
    size_t commitCalls = 0;
    size_t cancelCalls = 0;
    size_t destructionCalls = 0;
    bool committed = false;
    bool abandonedOnDestruction = false;
    bool failWrite = false;
    bool failCommit = false;
    bool failCancel = false;
};

class PrintJob final : public IPrintJob {
public:
    explicit PrintJob(std::shared_ptr<JobState> state) : state_(std::move(state)) {}
    ~PrintJob() override {
        ++state_->destructionCalls;
        // Track ownership cleanup separately from explicit Cancel. Do not make
        // an uncommitted job look as though the session called Cancel.
        state_->abandonedOnDestruction = active_;
    }
    void Write(const char* data, size_t size) override {
        state_->events.push_back("write");
        if (state_->failWrite) throw std::runtime_error("fake Write failure");
        state_->data.append(data, size);
    }
    void SetDocumentName(const std::string& name) override {
        state_->events.push_back("name");
        state_->documentName = name;
    }
    void Cancel() override {
        state_->events.push_back("cancel");
        ++state_->cancelCalls;
        if (state_->failCancel) throw std::runtime_error("fake Cancel failure");
        active_ = false;
    }
    void Commit() override {
        state_->events.push_back("commit");
        ++state_->commitCalls;
        if (state_->failCommit) throw std::runtime_error("fake Commit failure");
        state_->committed = true;
        active_ = false;
    }
    size_t GetBytesWritten() const override { return state_->data.size(); }

private:
    std::shared_ptr<JobState> state_;
    bool active_ = true;
};

class JobFactory final : public IPrinterJobFactory {
public:
    std::unique_ptr<IPrintJob> Create(const std::string& printer, const std::string& document) override {
        const auto call = createCalls++;
        if (call == failCreateAt) throw std::runtime_error("fake Create failure");
        auto state = std::make_shared<JobState>();
        state->printerName = printer;
        state->initialDocumentName = state->documentName = document;
        state->failWrite = failWrite;
        state->failCommit = failCommit;
        state->failCancel = failCancel;
        jobs.push_back(state);
        return std::make_unique<PrintJob>(std::move(state));
    }

    std::vector<std::shared_ptr<JobState>> jobs;
    size_t createCalls = 0;
    size_t failCreateAt = (std::numeric_limits<size_t>::max)();
    bool failWrite = false;
    bool failCommit = false;
    bool failCancel = false;
};

class LpdTransport final : public ILpdTransport {
public:
    explicit LpdTransport(std::string input, size_t chunk = 2)
        : input_(std::move(input)), chunk_(chunk) {
        if (chunk_ == 0) throw std::invalid_argument("Fake transport chunk must be positive");
    }
    LpdEndpoint Endpoint() const override { return {"127.0.0.1", 721}; }
    void WriteByte(char value) override {
        events.push_back(value == 0 ? "ack" : "nak");
        responses.push_back(value);
    }
    void ReadLine(std::string& line) override {
        ++readLineCalls;
        events.push_back("readline");
        const auto end = input_.find('\n', offset_);
        if (end == std::string::npos) throw LpdTransportDisconnected(Endpoint());
        line = input_.substr(offset_, end - offset_);
        offset_ = end + 1;
    }
    void ReadExactly(size_t size, const BufferCallback& callback) override {
        exactReads.push_back(size);
        events.push_back("read_exact:" + std::to_string(size));
        size_t remaining = size;
        while (remaining != 0) {
            if (offset_ == input_.size()) throw LpdTransportDisconnected(Endpoint());
            const auto count = (std::min)(remaining, (std::min)(chunk_, input_.size() - offset_));
            callback(input_.data() + offset_, count);
            offset_ += count;
            remaining -= count;
            payloadBytesRead += count;
        }
    }
    void ReadToEnd(const BufferCallback& callback) override {
        ++readToEndCalls;
        events.push_back("read_to_end");
        while (offset_ != input_.size()) {
            const auto count = (std::min)(chunk_, input_.size() - offset_);
            callback(input_.data() + offset_, count);
            offset_ += count;
            payloadBytesRead += count;
        }
    }
    size_t Remaining() const { return input_.size() - offset_; }

    std::string responses;
    // Interleaved write/read trace, so tests can verify handshake ordering
    // (for example that a file-command ACK precedes reading the file body).
    std::vector<std::string> events;
    std::vector<size_t> exactReads;
    size_t readLineCalls = 0;
    size_t readToEndCalls = 0;
    size_t payloadBytesRead = 0;

private:
    std::string input_;
    size_t chunk_;
    size_t offset_ = 0;
};

class RawTransport final : public IRawPrintTransport {
public:
    explicit RawTransport(std::vector<std::string> chunks) : chunks_(std::move(chunks)) {}
    void ReadToEnd(const BufferCallback& callback) override {
        ++reads;
        for (const auto& chunk : chunks_) callback(chunk.data(), chunk.size());
        if (failAfterRead) throw std::runtime_error("fake RAW read failure");
    }
    size_t reads = 0;
    bool failAfterRead = false;

private:
    std::vector<std::string> chunks_;
};

} // namespace test_support
