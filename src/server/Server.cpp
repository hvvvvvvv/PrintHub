#include <Server.h>

#pragma warning(disable:4996)

PrintServer::PrintServer(uint16_t port, int minThreads) {
	m_shutdown = true;
	m_listenAddr = { 0 };
	m_listenAddr.sin_family = AF_INET;
	m_listenAddr.sin_addr.s_addr = INADDR_ANY;
	SetListenPort(port);
	SetMinThreads(minThreads);
}

PrintServer::~PrintServer() {
	Stop();
	if (m_listenSocket != INVALID_SOCKET) {
		closesocket(m_listenSocket);
	}
}

void PrintServer::SetListenAddr(const std::string& addr) {
	m_listenAddr.sin_addr.s_addr = inet_addr(addr.c_str());
	if (m_listenAddr.sin_addr.s_addr == INADDR_NONE) {
		throw std::invalid_argument("Invalid IP address format");
	}
}

void PrintServer::SetListenPort(uint16_t port) {
	if (port == 0) {
		throw std::invalid_argument("Port number cannot be zero");
	}
	m_listenAddr.sin_port = htons(port);
}

void PrintServer::SetMinThreads(int minThreads) {
	if (minThreads < 1) {
		throw std::invalid_argument("Minimum threads must be at least 1");
	}
	m_minThreads = minThreads;
}

std::string PrintServer::GetListenAddr() const {
	return inet_ntoa(m_listenAddr.sin_addr);
}

uint16_t PrintServer::GetListenPort() const {
	return ntohs(m_listenAddr.sin_port);
}

int PrintServer::GetMinThreads() const {
	return m_minThreads;
}

void PrintServer::Start() {
	std::lock_guard<std::mutex> lock(m_statusMutex);
	if (!m_shutdown) {
		return;
	}	
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		throw std::system_error(WSAGetLastError(), std::system_category(), "WSAStartup failed");
	}

	m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_listenSocket == INVALID_SOCKET) {
		WSACleanup();
		throw std::system_error(WSAGetLastError(), std::system_category(), "Socket creation failed");
	}

	// 设置地址重用选项（防止 bind 失败）
	int yes = 1;
	if (setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
		reinterpret_cast<char*>(&yes), sizeof(yes)) == SOCKET_ERROR) {
		closesocket(m_listenSocket);
		WSACleanup();
		throw std::system_error(WSAGetLastError(), std::system_category(), "setsockopt failed");
	}

	if (bind(m_listenSocket, reinterpret_cast<sockaddr*>(&m_listenAddr), sizeof(m_listenAddr)) == SOCKET_ERROR) {
		closesocket(m_listenSocket);
		WSACleanup();
		throw std::system_error(WSAGetLastError(), std::system_category(), "Bind failed");
	}

	if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
		closesocket(m_listenSocket);
		WSACleanup();
		throw std::system_error(WSAGetLastError(), std::system_category(), "Listen failed");
	}

	m_workerThreads.clear();
	auto numThreads = min(m_minThreads, static_cast<int>(std::thread::hardware_concurrency()));
	
	for (int i = 0; i < m_minThreads; ++i) {
		m_workerThreads.emplace_back([this](std::stop_token st) {
			workerThread(st);
		});
	}
	m_acceptThread = std::jthread([this](std::stop_token st) {
		listenLoop(st);
	});
	m_shutdown = false;
	
}

void PrintServer::Stop() {
	std::lock_guard<std::mutex> lock(m_statusMutex);
	if (m_shutdown) {
		return;
	}
	// 关闭监听套接字（会中断 accept 调用）
	if (m_listenSocket != INVALID_SOCKET) {
		closesocket(m_listenSocket);
		m_listenSocket = INVALID_SOCKET;
	}

	m_acceptThread.request_stop(); // 请求停止接受线程
	m_acceptThread.join(); // 等待接受线程退出

	for (auto& thread : m_workerThreads) {
		thread.request_stop();
		thread.join();
	}
	m_workerThreads.clear();

	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		while (!m_taskQueue.empty()) {
			closesocket(std::get<SOCKET>(m_taskQueue.front())); // 关闭未处理的套接字
			m_taskQueue.pop();
		}
	}
	WSACleanup();
	m_shutdown = true;
}

void PrintServer::listenLoop(std::stop_token stopToken) {
	while (!stopToken.stop_requested()) {
		sockaddr_in clientAddr = { 0 };
		int addrLen = sizeof(clientAddr);
		SOCKET clientSocket = accept(m_listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
		if (clientSocket == INVALID_SOCKET) {
			int err = WSAGetLastError();
			if (err == WSAEINTR || err == WSAENOTSOCK) {
				break; // 正常关闭
			}
			continue; // 其他错误继续尝试
		}

		// 将新连接加入任务队列
		{
			std::lock_guard<std::mutex> lock(m_queueMutex);
			m_taskQueue.push(std::make_tuple(clientSocket, clientAddr));
		}
		m_condition.notify_one();
	}
}

void PrintServer::workerThread(std::stop_token stopToken) {
	while (!stopToken.stop_requested()) {
		SOCKET clientSocket = INVALID_SOCKET;
		sockaddr_in clientAddr = { 0 };

		// 等待新任务或关闭信号
		std::unique_lock<std::mutex> lock(m_queueMutex);
		auto hasData = m_condition.wait(lock, stopToken, [this]() {
			return !m_taskQueue.empty();
		});
		if (!hasData) {
			lock.unlock();
			continue; // 停止信号触发，退出线程
		}
		clientSocket = std::get<SOCKET>(m_taskQueue.front());
		clientAddr = std::get<sockaddr_in>(m_taskQueue.front());
		m_taskQueue.pop();
		lock.unlock();
		
		// 处理客户端连接
		try {
			auto client = TcpClient(clientSocket, clientAddr);
			handleClient(client);
		}
		catch (...) { }

		// clientSocket 的生命周期由 TcpClient 管理并在其析构函数中自动 closesocket
		clientSocket = INVALID_SOCKET;
	}
}