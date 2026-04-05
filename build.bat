@echo off
echo Building High Performance HTTP Server...

if not exist build mkdir build
cd build

echo Generating project files...
cmake ..

echo Building Release version...
cmake --build . --config Release

echo Build completed!
echo Executable location: build\Release\http_server.exe

pause