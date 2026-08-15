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
#include <set>
#include <tuple>
#include <vector>

class PrintServer {
	public:
		PrintServer& operator=(const PrintServer&) = delete;
		PrintServer(const PrintServer&) = delete;
		PrintServer() = delete;
		virtual ~PrintServer();
		void Start();
		void Stop();
		void SetListenPort(uint16_t port);
		void SetListenAddr(const std::string& addr);
		void SetMinThreads(int minThreads);
		std::string GetListenAddr() const;
		uint16_t GetListenPort() const;
		int GetMinThreads() const;
	protected:
		PrintServer(uint16_t port, int minThreads = 1);
		
		virtual void handleClient(TcpClient& client) = 0;
	private:
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
		std::queue<PendingClient> m_taskQueue;
		std::set<SOCKET> m_activeSockets;
		std::mutex m_queueMutex;
		std::mutex m_activeSocketsMutex;
		std::condition_variable_any m_condition;
		bool m_shutdown;
		std::mutex m_statusMutex;
};

class LPD : public PrintServer {
	public:
		LPD(uint16_t port, int minThreads = 1);
	protected:
		void handleClient(TcpClient& client) override;
};

class RawPrinting : public PrintServer {
	public:
		RawPrinting(std::string printerName, uint16_t port, int minThreads = 1);
	private:
		void handleClient(TcpClient& client) override;
		std::string m_printerName;
};
