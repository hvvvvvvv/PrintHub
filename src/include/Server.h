#pragma once

#include <cstdint>
#include <TcpClient.h>
#include <SocketHandle.h>
#include <string>
#include <thread>
#include <queue>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <set>
#include <tuple>
#include <vector>
#include <ServiceConfiguration.hpp>
#include <BoundedQueue.hpp>

class PrintServer {
	public:
		PrintServer& operator=(const PrintServer&) = delete;
		PrintServer(const PrintServer&) = delete;
		PrintServer() = delete;
		virtual ~PrintServer();
		void Start();
		void Stop();
		void SetListenAddr(const std::string& addr);
		std::string GetListenAddr() const;
		uint16_t GetListenPort() const;
	protected:
		PrintServer(uint16_t port, int minThreads = 1, int maxPendingClients = ServiceConfiguration::kDefaultMaxPendingClients);
		
		virtual void handleClient(TcpClient& client) = 0;
	private:
		void SetListenPort(uint16_t port);
		void SetMinThreads(int minThreads);
		void stopLocked();
		void listenLoop(std::stop_token stopToken, SOCKET listenSocket);
		void workerThread(std::stop_token stopToken);

		sockaddr_in m_listenAddr;
		int m_minThreads;
		struct PendingClient {
			UniqueSocket socket;
			sockaddr_in address;
		};

		UniqueSocket m_listenSocket;

		std::jthread m_acceptThread;
		std::vector<std::jthread> m_workerThreads;
		BoundedQueue<PendingClient> m_taskQueue;
		std::set<SOCKET> m_activeSockets;
		std::mutex m_queueMutex;
		std::mutex m_activeSocketsMutex;
		std::condition_variable_any m_condition;
		bool m_shutdown;
		std::atomic<bool> m_stopping = false;
		uint64_t m_rejectedPendingClients = 0;
		std::chrono::steady_clock::time_point m_lastQueueRejectionLog{};
		std::mutex m_statusMutex;
};

class LPD : public PrintServer {
	public:
		LPD(uint16_t port, std::vector<LpdQueueMapping> queueMappings, int minThreads = 1,
			size_t maxControlFileBytes = ServiceConfiguration::kDefaultLpdMaxControlFileBytes,
			int maxPendingClients = ServiceConfiguration::kDefaultMaxPendingClients);
	protected:
		void handleClient(TcpClient& client) override;
	private:
		std::vector<LpdQueueMapping> m_queueMappings;
		size_t m_maxControlFileBytes;
};

class RawPrinting : public PrintServer {
	public:
		RawPrinting(std::string printerName, uint16_t port, int minThreads = 1,
			int maxPendingClients = ServiceConfiguration::kDefaultMaxPendingClients);
	private:
		void handleClient(TcpClient& client) override;
		std::string m_printerName;
};
