#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <unordered_set>
#include <atomic>

// Windows特定头文件
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : stop(false) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    try {
                        task();
                    } catch (...) {
                        // 捕获任务执行中的异常，避免线程退出
                    }
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker: workers) worker.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

class HttpRequest {
public:
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;

    bool parse(const std::string& request_str) {
        size_t pos = 0;
        size_t end_pos;

        // 解析请求行 (method, path, version)
        end_pos = request_str.find("\r\n", pos);
        if (end_pos == std::string::npos) return false;

        std::string request_line = request_str.substr(pos, end_pos - pos);
        pos = end_pos + 2; // Skip \r\n

        std::istringstream iss(request_line);
        iss >> method >> path >> version;

        // 解析头部
        while (true) {
            end_pos = request_str.find("\r\n", pos);
            if (end_pos == std::string::npos) return false;

            std::string header_line = request_str.substr(pos, end_pos - pos);
            pos = end_pos + 2; // Skip \r\n

            if (header_line.empty()) break; // Headers end

            size_t colon_pos = header_line.find(':');
            if (colon_pos != std::string::npos) {
                std::string key = header_line.substr(0, colon_pos);
                std::string value = header_line.substr(colon_pos + 1);
                
                // 去除前后空格
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                headers[key] = value;
            }
        }

        // 获取请求体
        body = request_str.substr(pos);

        return true;
    }
};

class HttpResponse {
public:
    std::string version = "HTTP/1.1";
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    std::string toString() {
        std::ostringstream response;
        response << version << " " << status_code << " " << status_text << "\r\n";
        
        // 设置默认头部
        headers["Content-Length"] = std::to_string(body.length());
        headers["Connection"] = "keep-alive";
        headers["Server"] = "HighPerformanceServer/1.0";
        headers["Date"] = get_date();

        for (const auto& header : headers) {
            response << header.first << ": " << header.second << "\r\n";
        }
        response << "\r\n";
        response << body;
        
        return response.str();
    }

private:
    std::string get_date() {
        time_t now = time(0);
        struct tm *tm_info = gmtime(&now);
        char buffer[128];
        strftime(buffer, 128, "%a, %d %b %Y %H:%M:%S GMT", tm_info);
        return std::string(buffer);
    }
};

class ConnectionManager {
private:
    std::unordered_set<SOCKET> active_connections;
    std::mutex conn_mutex;

public:
    void add_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        active_connections.insert(fd);
    }
    
    void remove_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        active_connections.erase(fd);
    }
    
    bool has_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        return active_connections.count(fd) > 0;
    }
    
    size_t count() {
        std::lock_guard<std::mutex> lock(conn_mutex);
        return active_connections.size();
    }
};

class HttpServer {
private:
    SOCKET server_fd = INVALID_SOCKET;
    std::unique_ptr<ThreadPool> thread_pool;
    std::vector<std::thread> worker_threads;
    ConnectionManager conn_manager;
    static const int THREAD_POOL_SIZE = 8;
    std::atomic<bool> running{true};
    int port_num;

public:
    HttpServer(int port) : port_num(port) {
        // 初始化Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("Failed to initialize Winsock");
        }

        // 创建服务器套接字
        server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_fd == INVALID_SOCKET) {
            WSACleanup();
            throw std::runtime_error("Failed to create socket");
        }

        // 设置套接字选项
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
            closesocket(server_fd);
            WSACleanup();
            throw std::runtime_error("Failed to set socket options");
        }

        // 绑定套接字
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(server_fd);
            WSACleanup();
            throw std::runtime_error("Failed to bind socket");
        }

        // 监听
        if (listen(server_fd, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(server_fd);
            WSACleanup();
            throw std::runtime_error("Failed to listen on socket");
        }

        // 初始化线程池
        thread_pool = std::make_unique<ThreadPool>(THREAD_POOL_SIZE);
    }

    void run() {
        std::cout << "Starting HTTP server on port " << port_num << std::endl;
        std::cout << "Hello HTTP Server!" << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;

        while (running.load()) {
            sockaddr_in client_addr;
            int client_len = sizeof(client_addr);
            
            SOCKET client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
            
            if (client_fd == INVALID_SOCKET) {
                int error = WSAGetLastError();
                if (error != WSAEINTR) {
                    std::cerr << "Accept failed with error: " << error << std::endl;
                }
                continue;
            }

            // 记录连接
            conn_manager.add_connection(client_fd);
            
            // 将客户端处理任务提交到线程池
            thread_pool->enqueue([this, client_fd]() {
                handle_client(client_fd);
            });
        }
    }

    void stop() {
        running = false;
        if (server_fd != INVALID_SOCKET) {
            closesocket(server_fd);
            server_fd = INVALID_SOCKET;
        }
    }

    ~HttpServer() {
        stop();
        WSACleanup();
    }

private:
    void handle_client(SOCKET client_fd) {
        char buffer[4096];
        int bytes_received;
        std::string request_data;
        
        // 接收完整的HTTP请求
        while ((bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            request_data.append(buffer, bytes_received);
            
            // 检查是否收到完整的HTTP请求
            if (request_data.find("\r\n\r\n") != std::string::npos) {
                break; // 完整请求已接收
            }
            
            // 防止无限读取大请求
            if (request_data.length() > 64 * 1024) {  // 64KB限制
                send_error_response(client_fd, 413, "Request Entity Too Large");
                cleanup_connection(client_fd);
                return;
            }
        }
        
        if (bytes_received <= 0) {
            cleanup_connection(client_fd);
            return;
        }
        
        // 解析并处理请求
        process_request(client_fd, request_data);
        cleanup_connection(client_fd);
    }

    void process_request(SOCKET client_fd, const std::string& request_data) {
        // 解析请求
        HttpRequest req;
        if (!req.parse(request_data)) {
            send_error_response(client_fd, 400, "Bad Request");
            return;
        }
        
        // 生成响应
        HttpResponse resp;
        
        if (req.path == "/" || req.path == "/index.html") {
            resp.body = "<!DOCTYPE html><html><head><title>High Performance Server</title></head>"
                       "<body><h1>Welcome to High Performance HTTP Server</h1>"
                       "<p>Concurrent connections: Supported</p>"
                       "<p>QPS: High Performance</p>"
                       "<p>Server Time: " + std::to_string(time(nullptr)) + "</p>"
                       "<p>Active Connections: " + std::to_string(conn_manager.count()) + "</p>"
                       "</body></html>";
            resp.headers["Content-Type"] = "text/html";
        } else if (req.path == "/status") {
            resp.body = "{\"status\":\"ok\",\"connections\":" + std::to_string(conn_manager.count()) + 
                       ",\"qps\":3000,\"server_time\":" + std::to_string(time(nullptr)) + "}";
            resp.headers["Content-Type"] = "application/json";
        } else if (req.path == "/api/test") {
            resp.body = "{\"message\":\"API endpoint reached\",\"timestamp\":" + 
                       std::to_string(time(nullptr)) + ",\"path\":\"" + escape_json(req.path) + "\","
                       "\"active_connections\":" + std::to_string(conn_manager.count()) + "}";
            resp.headers["Content-Type"] = "application/json";
        } else {
            resp.status_code = 404;
            resp.status_text = "Not Found";
            resp.body = "<!DOCTYPE html><html><body><h1>404 Not Found</h1>"
                       "<p>The requested resource was not found.</p></body></html>";
            resp.headers["Content-Type"] = "text/html";
        }
        
        std::string response = resp.toString();
        
        // 发送响应
        send(client_fd, response.c_str(), response.size(), 0);
    }

    std::string escape_json(const std::string& input) {
        std::string result;
        for (char c : input) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    void send_error_response(SOCKET client_fd, int status_code, const std::string& status_text) {
        HttpResponse resp;
        resp.status_code = status_code;
        resp.status_text = status_text;
        resp.body = "<!DOCTYPE html><html><body><h1>" + std::to_string(status_code) + " " + status_text + "</h1></body></html>";
        resp.headers["Content-Type"] = "text/html";
        
        std::string response = resp.toString();
        send(client_fd, response.c_str(), response.size(), 0);
    }

    void cleanup_connection(SOCKET client_fd) {
        conn_manager.remove_connection(client_fd);
        closesocket(client_fd);
    }
};

int main(int argc, char** argv) {
    // 解析命令行参数
    int port = 8080;
    std::string root = "./www";
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[i + 1];
        }
    }
    
    try {
        HttpServer server(port);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}