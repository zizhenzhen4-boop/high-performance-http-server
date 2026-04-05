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
#include <chrono>
#include <future>
#include <fstream>
#include <regex>
#include <cstring>

// Windows特定头文件
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#define ioctlsocket ioctl
#define GetLastError() errno
#define Sleep(x) usleep(x * 1000)
#endif

// 跨平台兼容定义
#ifndef _WIN32
typedef int BOOL;
typedef unsigned long DWORD;
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINTR EINTR
#define MAKEWORD(a,b) (a)
#define WSAStartup(x,y) (0)
#define WSACleanup()
#define WSAGetLastError() errno
typedef struct linger LINGER;
#define SD_SEND SHUT_WR
#endif

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
    auto enqueue(F&& f) -> std::future<typename std::result_of<F()>::type> {
        using return_type = typename std::result_of<F()>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::forward<F>(f)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if(stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
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
        headers["Server"] = "HighPerformanceServer/2.0";
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
    std::atomic<size_t> total_requests{0};
    std::atomic<size_t> current_connections{0};

public:
    void add_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        active_connections.insert(fd);
        current_connections++;
    }
    
    void remove_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        active_connections.erase(fd);
        current_connections--;
    }
    
    bool has_connection(SOCKET fd) {
        std::lock_guard<std::mutex> lock(conn_mutex);
        return active_connections.count(fd) > 0;
    }
    
    size_t count() const {
        return current_connections.load();
    }
    
    size_t total_requests_count() const {
        return total_requests.load();
    }
    
    void increment_requests() {
        total_requests++;
    }
};

class RequestHandler {
public:
    virtual HttpResponse handle_request(const HttpRequest& req) = 0;
    virtual ~RequestHandler() = default;
};

class DefaultRequestHandler : public RequestHandler {
private:
    std::string document_root;
    ConnectionManager* conn_manager;
    
public:
    DefaultRequestHandler(const std::string& root, ConnectionManager* manager) 
        : document_root(root), conn_manager(manager) {}
    
    HttpResponse handle_request(const HttpRequest& req) override {
        HttpResponse resp;
        
        if (req.path == "/" || req.path == "/index.html") {
            resp.body = generate_homepage();
            resp.headers["Content-Type"] = "text/html";
        } else if (req.path == "/status") {
            resp.body = generate_status_json();
            resp.headers["Content-Type"] = "application/json";
        } else if (req.path == "/api/test") {
            resp.body = generate_api_test_json(req);
            resp.headers["Content-Type"] = "application/json";
        } else if (req.path == "/metrics") {
            resp.body = generate_metrics_json();
            resp.headers["Content-Type"] = "application/json";
        } else if (req.path == "/favicon.ico") {
            // 返回空响应或简单的图标
            resp.body = "";
            resp.headers["Content-Type"] = "image/x-icon";
        } else {
            // 尝试从文件系统加载静态文件
            std::string file_path = document_root + req.path;
            std::replace(file_path.begin(), file_path.end(), '\\', '/');
            
            if (load_static_file(file_path, resp)) {
                return resp;
            } else {
                resp.status_code = 404;
                resp.status_text = "Not Found";
                resp.body = "<!DOCTYPE html><html><body><h1>404 Not Found</h1>"
                           "<p>The requested resource was not found.</p></body></html>";
                resp.headers["Content-Type"] = "text/html";
            }
        }
        
        return resp;
    }
    
private:
    std::string generate_homepage() {
        std::stringstream ss;
        ss << "<!DOCTYPE html><html><head><title>High Performance Server</title>"
           << "<style>body { font-family: Arial, sans-serif; margin: 40px; background-color: #f0f0f0; }"
           << ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
           << "h1 { color: #2c3e50; }"
           << ".stats { display: flex; justify-content: space-around; margin: 20px 0; }"
           << ".stat-box { text-align: center; padding: 15px; background: #ecf0f1; border-radius: 5px; }"
           << ".stat-value { font-size: 24px; font-weight: bold; color: #3498db; }"
           << "</style></head>"
           << "<body><div class='container'>"
           << "<h1>Welcome to High Performance HTTP Server</h1>"
           << "<div class='stats'>"
           << "<div class='stat-box'><div class='stat-value'>" << conn_manager->count() << "</div><div>Active Connections</div></div>"
           << "<div class='stat-box'><div class='stat-value'>" << conn_manager->total_requests_count() << "</div><div>Total Requests</div></div>"
           << "<div class='stat-box'><div class='stat-value'>" << std::to_string(time(nullptr)) << "</div><div>Server Time</div></div>"
           << "</div>"
           << "<p>Concurrent connections: Supported</p>"
           << "<p>Thread Pool: Optimized for high QPS</p>"
           << "<p>Endpoints: /, /status, /api/test, /metrics</p>"
           << "</div></body></html>";
        return ss.str();
    }
    
    std::string generate_status_json() {
        std::stringstream ss;
        ss << "{"
           << "\"status\":\"ok\","
           << "\"connections\":" << conn_manager->count() << ","
           << "\"total_requests\":" << conn_manager->total_requests_count() << ","
           << "\"qps\": \"dynamic\","
           << "\"server_time\":" << std::to_string(time(nullptr)) << ","
           << "\"version\":\"2.0\""
           << "}";
        return ss.str();
    }
    
    std::string generate_api_test_json(const HttpRequest& req) {
        std::stringstream ss;
        ss << "{"
           << "\"message\":\"API endpoint reached\","
           << "\"timestamp\":" << std::to_string(time(nullptr)) << ","
           << "\"path\":\"" << escape_json(req.path) << "\","
           << "\"method\":\"" << req.method << "\","
           << "\"active_connections\":" << conn_manager->count() << ","
           << "\"headers\":" << headers_to_json(req.headers)
           << "}";
        return ss.str();
    }
    
    std::string generate_metrics_json() {
        std::stringstream ss;
        ss << "{"
           << "\"active_connections\":" << conn_manager->count() << ","
           << "\"total_requests\":" << conn_manager->total_requests_count() << ","
           << "\"server_time\":" << std::to_string(time(nullptr)) << ","
           << "\"uptime_seconds\":" << std::to_string(time(nullptr) - 1700000000)  // 示例启动时间
           << "}";
        return ss.str();
    }
    
    std::string headers_to_json(const std::map<std::string, std::string>& headers) {
        std::stringstream ss;
        ss << "{";
        bool first = true;
        for (const auto& header : headers) {
            if (!first) ss << ",";
            ss << "\"" << escape_json(header.first) << "\":\"" << escape_json(header.second) << "\"";
            first = false;
        }
        ss << "}";
        return ss.str();
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
    
    bool load_static_file(const std::string& file_path, HttpResponse& resp) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }
        
        // 读取文件内容
        std::stringstream buffer;
        buffer << file.rdbuf();
        resp.body = buffer.str();
        
        // 根据扩展名设置Content-Type
        std::string ext = file_path.substr(file_path.find_last_of(".") + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == "html" || ext == "htm") {
            resp.headers["Content-Type"] = "text/html";
        } else if (ext == "css") {
            resp.headers["Content-Type"] = "text/css";
        } else if (ext == "js") {
            resp.headers["Content-Type"] = "application/javascript";
        } else if (ext == "json") {
            resp.headers["Content-Type"] = "application/json";
        } else if (ext == "png") {
            resp.headers["Content-Type"] = "image/png";
        } else if (ext == "jpg" || ext == "jpeg") {
            resp.headers["Content-Type"] = "image/jpeg";
        } else if (ext == "gif") {
            resp.headers["Content-Type"] = "image/gif";
        } else if (ext == "txt") {
            resp.headers["Content-Type"] = "text/plain";
        } else {
            resp.headers["Content-Type"] = "application/octet-stream";
        }
        
        return true;
    }
};

class HttpServer {
private:
    SOCKET server_fd = INVALID_SOCKET;
    std::unique_ptr<ThreadPool> thread_pool;
    ConnectionManager conn_manager;
    std::unique_ptr<DefaultRequestHandler> default_handler;
    static const int THREAD_POOL_SIZE = 16; // 增加线程池大小
    std::atomic<bool> running{false};
    int port_num;
    std::string document_root;

public:
    HttpServer(int port, const std::string& root = "./www") : port_num(port), document_root(root) {
        // 初始化Winsock
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("Failed to initialize Winsock");
        }
#endif

        // 创建服务器套接字
        server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_fd == INVALID_SOCKET) {
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to create socket");
        }

        // 设置套接字选项
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
            closesocket(server_fd);
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to set socket options");
        }

        // 设置非阻塞模式
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(server_fd, FIONBIO, &mode);
#else
        int flags = fcntl(server_fd, F_GETFL, 0);
        if (flags == -1) {
            closesocket(server_fd);
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to get socket flags");
        }
        if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            closesocket(server_fd);
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to set non-blocking mode");
        }
#endif

        // 绑定套接字
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(server_fd);
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to bind socket");
        }

        // 监听
        if (listen(server_fd, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(server_fd);
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("Failed to listen on socket");
        }

        // 初始化线程池
        thread_pool = std::make_unique<ThreadPool>(THREAD_POOL_SIZE);
        
        // 初始化请求处理器
        default_handler = std::make_unique<DefaultRequestHandler>(document_root, &conn_manager);
    }

    void run() {
        running = true;
        std::cout << "Starting HTTP server on port " << port_num << std::endl;
        std::cout << "Document root: " << document_root << std::endl;
        std::cout << "Thread pool size: " << THREAD_POOL_SIZE << std::endl;
        std::cout << "Available endpoints:" << std::endl;
        std::cout << "  - http://localhost:" << port_num << "/" << std::endl;
        std::cout << "  - http://localhost:" << port_num << "/status" << std::endl;
        std::cout << "  - http://localhost:" << port_num << "/api/test" << std::endl;
        std::cout << "  - http://localhost:" << port_num << "/metrics" << std::endl;
        std::cout << "Press Ctrl+C to stop..." << std::endl;

        while (running.load()) {
            sockaddr_in client_addr;
            int client_len = sizeof(client_addr);
            
            SOCKET client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
            
            if (client_fd == INVALID_SOCKET) {
                int error = GetLastError();
#ifdef _WIN32
                if (error != WSAEWOULDBLOCK && error != WSAEINTR) {
                    std::cerr << "Accept failed with error: " << error << std::endl;
                }
#else
                if (error != EAGAIN && error != EWOULDBLOCK) {
                    std::cerr << "Accept failed with error: " << error << std::endl;
                }
#endif
                // 避免忙等待，短暂休眠
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
#ifdef _WIN32
        WSACleanup();
#endif
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
        
        // 记录请求
        conn_manager.increment_requests();
        
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
        
        // 使用请求处理器处理请求
        HttpResponse resp = default_handler->handle_request(req);
        
        std::string response = resp.toString();
        
        // 发送响应
        send(client_fd, response.c_str(), response.size(), 0);
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
        HttpServer server(port, root);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}