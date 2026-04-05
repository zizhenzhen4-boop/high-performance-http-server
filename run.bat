@echo off
echo Starting High Performance HTTP Server on port 9000...

if not exist build\Release\http_server.exe (
    echo Server executable not found. Please build first.
    pause
    exit /b 1
)

cd build\Release
start http://localhost:9000
http_server.exe --port 9000 --root ..\..\www

pause