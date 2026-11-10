#include <LpdSession.h>

#include <spdlog/spdlog.h>

#include <limits>
#include <set>
#include <utility>

namespace {

enum class LpdCommand : char {
    PrintWaitingJobs = 0x01,
    ReceiveJob = 0x02,
    SendQueueStateShort = 0x03,
    SendQueueStateLong = 0x04,
    RemoveJobs = 0x05,
};

enum class ReceiveJobCommand : char {
    AbortJob = 0x01,
    ReceiveControlFile = 0x02,
    ReceiveDataFile = 0x03,
};

enum class LpdResponse : char {
    Acknowledged = 0x00,
    Rejected = 0x01,
};

constexpr size_t kCommandCodeIndex = 0;
constexpr size_t kFirstCommandArgumentIndex = 1;
constexpr size_t kSingleByte = 1;
constexpr size_t kNoBytesReceived = 0;
constexpr size_t kMinimumTopLevelCommandSize = 2;
constexpr size_t kMinimumFileCommandSize = 4;
constexpr uint64_t kUnknownDataFileSize = 0;
constexpr char kCommandArgumentSeparator = ' ';
constexpr char kFirstPrintControlCode = 'a';
constexpr char kLastPrintControlCode = 'z';
constexpr char kControlHostName = 'H';
constexpr char kControlUserName = 'P';
constexpr char kControlJobName = 'J';
constexpr char kControlSourceFileName = 'N';
constexpr char kControlBannerUser = 'L';
constexpr char kDefaultLpdDocumentName[] = "lpr_job";

struct FileCommand {
    uint64_t size;
    std::string name;
};

constexpr char toChar(LpdResponse response) {
    return static_cast<char>(response);
}

std::optional<FileCommand> parseFileCommand(const std::string& command) {
    if (command.size() < kMinimumFileCommandSize) {
        return std::nullopt;
    }
    const auto separator = command.find(kCommandArgumentSeparator, kFirstCommandArgumentIndex);
    if (separator == std::string::npos || separator == command.size() - kFirstCommandArgumentIndex) {
        return std::nullopt;
    }
    const auto countText = command.substr(kFirstCommandArgumentIndex, separator - kFirstCommandArgumentIndex);
    uint64_t size = 0;
    try {
        size_t consumed = 0;
        size = std::stoull(countText, &consumed);
        if (consumed != countText.size()) {
            return std::nullopt;
        }
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
    return FileCommand{ size, command.substr(separator + kFirstCommandArgumentIndex) };
}

bool isPrintFileReference(char code) {
    return code >= kFirstPrintControlCode && code <= kLastPrintControlCode;
}

void readTerminator(ILpdTransport& transport) {
    char terminator = toChar(LpdResponse::Acknowledged);
    transport.ReadExactly(kSingleByte, [&terminator](const char* data, size_t size) {
        if (size == kSingleByte) {
            terminator = data[0];
        }
    });
    if (terminator != toChar(LpdResponse::Acknowledged)) {
        throw std::runtime_error("LPD file terminator is not NUL");
    }
}

void reject(ILpdTransport& transport) {
    transport.WriteByte(toChar(LpdResponse::Rejected));
}

} // namespace

LpdSession::LpdSession(IPrinterJobFactory& jobFactory, PrinterNameResolver printerNameResolver, size_t maxControlFileBytes)
    : m_jobFactory(jobFactory), m_printerNameResolver(std::move(printerNameResolver)),
      m_maxControlFileBytes(maxControlFileBytes) {
    if (m_maxControlFileBytes == 0) throw std::invalid_argument("LPD maximum control file bytes must be at least 1");
}

void LpdSession::Run(ILpdTransport& transport) {
    const auto endpoint = transport.Endpoint();
    try {
        std::string command;
        transport.ReadLine(command);
        if (command.size() < kMinimumTopLevelCommandSize) {
            spdlog::warn("Invalid LPD command from {}:{}", endpoint.address, endpoint.port);
            reject(transport);
            return;
        }

        const auto commandCode = static_cast<LpdCommand>(command[kCommandCodeIndex]);
        const auto queueName = command.substr(kFirstCommandArgumentIndex);
        switch (commandCode) {
        case LpdCommand::PrintWaitingJobs:
            spdlog::info("LPD print-waiting-jobs request for '{}' from {}:{}", queueName, endpoint.address, endpoint.port);
            transport.WriteByte(toChar(LpdResponse::Acknowledged));
            return;
        case LpdCommand::ReceiveJob:
            handleReceiveJob(queueName, transport);
            return;
        case LpdCommand::SendQueueStateShort:
        case LpdCommand::SendQueueStateLong:
        case LpdCommand::RemoveJobs:
            spdlog::warn("Unsupported LPD management command {:02x} from {}:{}",
                static_cast<unsigned char>(command[kCommandCodeIndex]), endpoint.address, endpoint.port);
            reject(transport);
            return;
        default:
            spdlog::warn("Invalid LPD command {:02x} from {}:{}",
                static_cast<unsigned char>(command[kCommandCodeIndex]), endpoint.address, endpoint.port);
            reject(transport);
            return;
        }
    }
    catch (const std::exception& error) {
        spdlog::warn("LPD session from {}:{} ended with error: {}", endpoint.address, endpoint.port, error.what());
    }
}

void LpdSession::handleReceiveJob(const std::string& queueName, ILpdTransport& transport) {
    const auto endpoint = transport.Endpoint();
    const auto printerName = m_printerNameResolver(queueName);
    spdlog::info("LPD receive-job request for queue '{}' from {}:{}", queueName, endpoint.address, endpoint.port);
    if (!printerName.has_value()) {
        spdlog::error("No printer matches LPD queue '{}' from {}:{}", queueName, endpoint.address, endpoint.port);
        reject(transport);
        return;
    }

    std::unique_ptr<IPrintJob> job;
    try {
        job = m_jobFactory.Create(*printerName, kDefaultLpdDocumentName);
    }
    catch (const std::exception& error) {
        spdlog::error("Failed to create LPD printer job for {}:{}: {}", endpoint.address, endpoint.port, error.what());
        reject(transport);
        return;
    }

    transport.WriteByte(toChar(LpdResponse::Acknowledged));
    std::set<std::string> expectedDataFiles;
    std::set<std::string> receivedDataFiles;
    bool controlFileReceived = false;

    const auto commitIfComplete = [&]() -> bool {
        if (!controlFileReceived || expectedDataFiles.empty() || receivedDataFiles != expectedDataFiles) {
            return false;
        }
        job->Commit();
        spdlog::info("Completed LPD print job for {}:{} ({} bytes)", endpoint.address, endpoint.port, job->GetBytesWritten());
        return true;
    };

    while (true) {
        std::string command;
        try {
            transport.ReadLine(command);
        }
        catch (const LpdTransportDisconnected&) {
            spdlog::warn("LPD client {}:{} disconnected before a complete job was acknowledged", endpoint.address, endpoint.port);
            return;
        }

        if (command.empty()) {
            spdlog::warn("Empty LPD subcommand from {}:{}", endpoint.address, endpoint.port);
            reject(transport);
            return;
        }

        const auto commandCode = static_cast<ReceiveJobCommand>(command[kCommandCodeIndex]);
        if (commandCode == ReceiveJobCommand::AbortJob) {
            try {
                job->Cancel();
                job = m_jobFactory.Create(*printerName, kDefaultLpdDocumentName);
                expectedDataFiles.clear();
                receivedDataFiles.clear();
                controlFileReceived = false;
                transport.WriteByte(toChar(LpdResponse::Acknowledged));
            }
            catch (const std::exception& error) {
                spdlog::error("Failed to abort LPD job for {}:{}: {}", endpoint.address, endpoint.port, error.what());
                reject(transport);
                return;
            }
            continue;
        }

        const auto fileCommand = parseFileCommand(command);
        if (!fileCommand.has_value()) {
            spdlog::warn("Invalid LPD file subcommand from {}:{}", endpoint.address, endpoint.port);
            reject(transport);
            return;
        }

        if (commandCode == ReceiveJobCommand::ReceiveControlFile) {
            if (controlFileReceived || fileCommand->size > m_maxControlFileBytes ||
                fileCommand->size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
                reject(transport);
                return;
            }
            try {
                transport.WriteByte(toChar(LpdResponse::Acknowledged));
                std::string controlFile;
                controlFile.reserve(static_cast<size_t>(fileCommand->size));
                transport.ReadExactly(static_cast<size_t>(fileCommand->size), [&controlFile](const char* data, size_t size) {
                    controlFile.append(data, size);
                });
                readTerminator(transport);

                for (size_t lineStart = 0; lineStart < controlFile.size();) {
                    const auto lineEnd = controlFile.find('\n', lineStart);
                    const auto lineSize = (lineEnd == std::string::npos ? controlFile.size() : lineEnd) - lineStart;
                    if (lineSize != 0) {
                        const auto code = controlFile[lineStart];
                        const auto value = controlFile.substr(lineStart + kFirstCommandArgumentIndex,
                            lineSize - kFirstCommandArgumentIndex);
                        if (isPrintFileReference(code)) {
                            expectedDataFiles.insert(value);
                        }
                        else if (code == kControlJobName) {
                            job->SetDocumentName(value);
                        }
                        else if (code == kControlHostName || code == kControlUserName ||
                            code == kControlSourceFileName || code == kControlBannerUser) {
                            spdlog::debug("LPD control {}: {}", code, value);
                        }
                        else {
                            spdlog::debug("Ignoring LPD control {}: {}", code, value);
                        }
                    }
                    if (lineEnd == std::string::npos) {
                        break;
                    }
                    lineStart = lineEnd + kFirstCommandArgumentIndex;
                }
                if (expectedDataFiles.empty()) {
                    throw std::runtime_error("LPD control file contains no print-file reference");
                }
                for (const auto& received : receivedDataFiles) {
                    if (!expectedDataFiles.contains(received)) {
                        throw std::runtime_error("LPD control file does not reference received data file");
                    }
                }
                controlFileReceived = true;
                transport.WriteByte(toChar(LpdResponse::Acknowledged));
                if (commitIfComplete()) {
                    return;
                }
            }
            catch (const std::exception& error) {
                spdlog::warn("Invalid LPD control file from {}:{}: {}", endpoint.address, endpoint.port, error.what());
                try { reject(transport); } catch (...) {}
                return;
            }
            continue;
        }

        if (commandCode != ReceiveJobCommand::ReceiveDataFile || receivedDataFiles.contains(fileCommand->name) ||
            (controlFileReceived && !expectedDataFiles.contains(fileCommand->name))) {
            reject(transport);
            return;
        }

        try {
            transport.WriteByte(toChar(LpdResponse::Acknowledged));
            size_t receivedBytes = 0;
            const auto writeData = [&job, &receivedBytes](const char* data, size_t size) {
                job->Write(data, size);
                receivedBytes += size;
            };

            if (fileCommand->size == kUnknownDataFileSize ||
                fileCommand->size > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
                transport.ReadToEnd(writeData);
                receivedDataFiles.insert(fileCommand->name);
                if (commitIfComplete()) {
                    return;
                }
                spdlog::warn("LPD unknown-length data file '{}' ended before control-file completion from {}:{}",
                    fileCommand->name, endpoint.address, endpoint.port);
                return;
            }

            try {
                transport.ReadExactly(static_cast<size_t>(fileCommand->size), writeData);
            }
            catch (const LpdTransportDisconnected&) {
                if (receivedBytes == kNoBytesReceived) {
                    throw;
                }
                receivedDataFiles.insert(fileCommand->name);
                spdlog::warn("LPD byte-count compatibility fallback for '{}' from {}:{}: advertised {}, received {} before EOF",
                    fileCommand->name, endpoint.address, endpoint.port, fileCommand->size, receivedBytes);
                commitIfComplete();
                return;
            }

            readTerminator(transport);
            receivedDataFiles.insert(fileCommand->name);
            transport.WriteByte(toChar(LpdResponse::Acknowledged));
            if (commitIfComplete()) {
                return;
            }
        }
        catch (const std::exception& error) {
            spdlog::warn("Failed to receive LPD data file '{}' from {}:{}: {}",
                fileCommand->name, endpoint.address, endpoint.port, error.what());
            try { reject(transport); } catch (...) {}
            return;
        }
    }
}
