#include <cstdint>
#include <TcpClient.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

class PrintServer {
	public:
		PrintServer& operator=(const PrintServer&) = delete;
		PrintServer(const PrintServer&) = delete;
		PrintServer() = delete;
		~PrintServer();
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
		void listenLoop(std::stop_token stopToken);
		void workerThread(std::stop_token stopToken);

		sockaddr_in m_listenAddr;
		int m_minThreads;
		SOCKET m_listenSocket = INVALID_SOCKET;

		std::jthread m_acceptThread;
		std::vector<std::jthread> m_workerThreads;
		std::queue<std::tuple<SOCKET, sockaddr_in>> m_taskQueue;
		std::mutex m_queueMutex;
		std::condition_variable_any m_condition;
		bool m_shutdown;
		std::mutex m_statusMutex;
};

class LPD : public PrintServer {
	public:
		LPD(uint16_t port, int minThreads = 1);
	protected:
		void handleClient(TcpClient& client) override;
	private:
		
		void handleWaitingPrintJob(std::string& queueName, TcpClient& client);
};

class RawPrinting : public PrintServer {
	public:
		RawPrinting(std::string printerName, uint16_t port, int minThreads = 1);
	private:
		void handleClient(TcpClient& client) override;
		std::string m_printerName;
};

