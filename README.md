# High Performance HTTP Server

![C++](https://img.shields.io/badge/C++-17-blue)
![Windows](https://img.shields.io/badge/Platform-Windows-green)
![License](https://img.shields.io/badge/License-MIT-yellow)

A high-performance, multi-threaded HTTP server implementation in C++ for Windows, using Winsock2 and a thread pool for concurrent request handling.

## Features

- ✅ Multi-threaded architecture with thread pool
- ✅ Efficient connection management
- ✅ Built-in endpoints (`/`, `/status`, `/api/test`)
- ✅ HTTP/1.1 compliant responses
- ✅ Configurable port and root directory
- ✅ Error handling and logging
- ✅ Cross-platform build support (CMake)

## Getting Started

### Prerequisites

- Windows 10/11
- CMake 3.10+
- Visual Studio 2019+ or MinGW-w64

### Building

1. Clone the repository:
   ```bash
   git clone https://github.com/zizizhen4-Boop/high-performance-http-server.git
   cd high-performance-http-server
   ```

2. Create build directory and compile:
   ```bash
   mkdir build
   cd build
   cmake ..
   cmake --build . --config Release
   ```

### Running

Run the server with default settings (port 9000):
```bash
./Release/http_server.exe --port 9000 --root ../www
```

Or specify custom port and root directory:
```bash
./Release/http_server.exe --port 8080 --root ./my_website
```

### Endpoints

- `GET /` - Main page with server information
- `GET /status` - Returns server status in JSON format
- `GET /api/test` - Test API endpoint returning JSON data
- `GET /index.html` - Same as root endpoint

## Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--port` | 8080 | Port number to listen on |
| `--root` | ./www | Root directory for static files |

## Performance

- Handles multiple concurrent connections efficiently
- Thread pool size: 8 threads (configurable in code)
- Memory-efficient request parsing
- Low-latency response generation

## Project Structure

```
high-performance-http-server/
├── src/              # Source code
├── www/              # Static files directory
├── CMakeLists.txt    # Build configuration
├── README.md         # This file
└── LICENSE           # MIT License
```

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Author

- **zizizhen4-Boop** - [GitHub Profile](https://github.com/zizizhen4-Boop)

### Quick Start (Windows)

For quick setup and running:

1. **Build**: Double-click `build.bat`
2. **Run**: Double-click `run.bat`

This will automatically open your browser to `http://localhost:9000`.
