#include <Server.h>
#include <spdlog/spdlog.h>
#include <PrinterJob.h>
#include <string>
#include <memory>
#include <set>
#include <utility.h>
#include <config.h>

LPD::LPD(uint16_t port, int minThreads)
	: PrintServer(port, minThreads){
}

void LPD::handleClient(TcpClient& client) {
    std::string command;
    try {
        client.ReadLine(command);
    }
    catch (const std::system_error& e) {
        spdlog::error("System error reading LPR command from {}:{}. Error: {} Win32 error code: {}", client.Addr(), client.Port(), e.what(), e.code().value());
        return;
    }
    catch (const std::exception& e) {
        spdlog::error("Error reading LPR command from {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
        return;
    }

    if (command.size() < 2) {
        spdlog::error("Invalid LPR command received: {}", command);
        return;
    }

    auto commandCode = command[0];
    auto queueName = command.substr(1);

    switch (commandCode) {
    case 0x01: {
        try {
            client.Write(0x00);
            spdlog::info("Received LPR Print any waiting jobs from {}:{} with queue name: {}", client.Addr(), client.Port(), queueName);
        }
        catch (const std::exception& e) {
            spdlog::error("Error sending response to LPR command from {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
            return;
        }
    } break;
    case 0x02: {
        try {
            handleWaitingPrintJob(queueName, client);
        }
        catch (const std::exception& e) {
            spdlog::error(e.what());
        }
    } break;
    case 0x03:
    case 0x04:
    case 0x05: {
        spdlog::warn("LPR command code {:x} is not implemented yet", commandCode);
    } break;
    default: {
        spdlog::error("Invalid LPR command code received: {:x}", commandCode);
    }

    }
}

void LPD::handleWaitingPrintJob(std::string& queueName, TcpClient& client) {
    
    std::unique_ptr<PrinterJob> job = nullptr;
    auto printerName = utility::GetPrinterNameFromShareName(queueName);
    spdlog::info("Received waiting print jobs request for queue: {} from {}:{}", queueName, client.Addr(), client.Port());

    if (!printerName.has_value()) {
        spdlog::error("No printer found for queue: {} from {}:{}", queueName, client.Addr(), client.Port());
        client.Write(0x01);
        return;
    }

    try {
        job = std::make_unique<PrinterJob>(printerName.value(), "waiting_job");
    }
    catch (const PrinterJobError& e) {
        spdlog::error("Failed to create printer job for {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
        client.Write(0x01);
        return;
    }

    client.Write(0x00);
    auto dataFiles = std::make_unique<std::set<std::string>>();
    auto controlFileReceived = false;
    while (true) {
        std::string command;
        try {
            client.ReadLine(command);
        }
        catch (TcpClientDisconnected) {
            if (job->GetBytesWritten() > 0) {
                spdlog::info("Start printing job for {}:{}", client.Addr(), client.Port());
                job->Print();
                spdlog::info("Print job completed for {}:{}", client.Addr(), client.Port());
            }
            else {
                spdlog::warn("Client {}:{} disconnected before sending any data", client.Addr(), client.Port());
            }
            return;
        }
        if (command.empty()) {
            spdlog::warn("Received empty command from {}:{}", client.Addr(), client.Port());
            continue;
        }
        auto commandCode = command[0];
        switch (commandCode) {
        case 0x01: {
            spdlog::info("Abort job request received from {}:{}", client.Addr(), client.Port());
            try {
                controlFileReceived = false;
                dataFiles->clear();
                job->Clear();
                spdlog::info("Job cancelled successfully for {}:{}", client.Addr(), client.Port());
            }
            catch (const std::exception& e) {
                spdlog::error("Error cancelling job for {}:{}. Error: {}", client.Addr(), client.Port(), e.what());
                client.Write(0x01);
                return;
            }
        } break;
        case 0x02: {
            spdlog::info("Receive print job request control file head from {}:{}", client.Addr(), client.Port());
            auto commandParms = command.substr(1);
            if (commandParms.empty()) {
                spdlog::error("Invalid command parameters for receive print job from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                return;
            }
            auto it = commandParms.find(' ');
            if (it == std::string::npos || it == commandParms.size() - 1) {
                spdlog::error("Invalid command parameters for receive print job from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                return;
            }
            auto fileName = commandParms.substr(it + 1);
            auto fileSize = utility::StringToUint64(commandParms.substr(0, it));
            if (!fileSize.has_value() || fileSize.value() > MAX_RECV_SIZE) {
                spdlog::error("Invalid file size parameter for receive print job from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                return;
            }
            spdlog::info("Receiving print job control file head: {} with size: {} from {}:{}", fileName, fileSize.value(), client.Addr(), client.Port());
            if (controlFileReceived) {
                spdlog::error("Control file already received from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                return;
            }
            client.Write(0x00);
            std::string controlFileData;
            controlFileReceived = true;
            auto _dataFiles = std::make_unique<std::set<std::string>>();
            client.Read(fileSize.value() + 1, [&controlFileData](const char* data, size_t size) {
                controlFileData.append(data, size);
                });
            controlFileData.pop_back();
            spdlog::info("Received print job control file head: {} succussed from {}:{}", fileName, client.Addr(), client.Port());
            for (const auto& line : utility::SplistByLine(controlFileData)) {
                spdlog::debug("Control file line: {}", line);
                if (line.empty()) continue;
                switch (line[0]) {
                    case 'H': {
                        spdlog::info("Received host name: {}", line.substr(1));
                    } break;
                    case 'P': {
                        spdlog::info("Received user name: {}", line.substr(1));
                    } break;
                    case 'J': {
                        spdlog::info("Received job name: {}", line.substr(1));
                        job->SetDocName(line.substr(1));
                    } break;
                    case 'l': {
                        auto datafile = line.substr(1);
                        spdlog::info("data file name {} defined in control file: {}", line.substr(1), datafile);
                        _dataFiles->insert(datafile);
                    } break;
                    case 'f':
                    case 'o':
                    case 'p':
                    case 't':
                    case '1':
                    case '2':
                    case 'L': {
                        spdlog::error("Unsupported control file parameter: {} from {}:{}", line, client.Addr(), client.Port());
                        client.Write(0x01);
                        return;
                    }
                }
            }
                
                if (_dataFiles->empty()) {
                    spdlog::error("No data files defined in control file from {}:{}", client.Addr(), client.Port());
                    client.Write(0x01);
                    return;
                }
                for (const auto& dataFile : *dataFiles) {
                    if (!_dataFiles->contains(dataFile)) {
                        spdlog::error("Data file {} not found in control file from {}:{}", dataFile, client.Addr(), client.Port());
                        client.Write(0x01);
                        return;
                    }
                }
                dataFiles = std::move(_dataFiles);
                client.Write(0x00);
            

        } break;
        case 0x03: {
            spdlog::info("Receive print job data request from {}:{}", client.Addr(), client.Port());
            auto commandParms = command.substr(1);
            if (commandParms.empty()) {
                spdlog::error("Invalid command parameters for receive print job data from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                break;
            }
            auto it = commandParms.find(' ');
            if (it == std::string::npos || it == commandParms.size() - 1) {
                spdlog::error("Invalid command parameters for receive print job data from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                break;
            }
            auto fileName = commandParms.substr(it + 1);
            auto fileSize = utility::StringToNumber<size_t>(commandParms.substr(0, it));
            if (!fileSize.has_value()) {
                spdlog::error("Invalid file size parameter for receive print job data from {}:{}", client.Addr(), client.Port());
                client.Write(0x01);
                break;
            }
            spdlog::info("Receiving print job data file: {} with size: {} from {}:{}", fileName, fileSize.value(), client.Addr(), client.Port());
            if (controlFileReceived) {
                if (!dataFiles->contains(fileName)) {
                    spdlog::error("Data file {} not found in control file from {}:{}", fileName, client.Addr(), client.Port());
                    client.Write(0x01);
                    return;
                }
            }
            else {
                if (dataFiles->contains(fileName)) {
                    spdlog::error("Data file {} already received from {}:{}", fileName, client.Addr(), client.Port());
                    client.Write(0x01);
                    return;
                }
                dataFiles->insert(fileName);
            }
			client.Write(0x00);
            auto cb = [&job](const char* data, size_t size) {
                job->Write(const_cast<char*>(data), static_cast<DWORD>(size));
                };
            if (fileSize == 0 || fileSize > MAX_RECV_SIZE) {
                client.ReadAll(cb);
            }
            else {
                client.Read(static_cast<int>(fileSize.value()), cb);
                client.Read(1, nullptr);
                client.Write(0x00);
            }
            spdlog::info("Received print job data file: {} succussed from {}:{}", fileName, client.Addr(), client.Port());

        } break;
        }

    }
}