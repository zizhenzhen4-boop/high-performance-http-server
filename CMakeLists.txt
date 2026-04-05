cmake_minimum_required(VERSION 3.10)
project(high_performance_http_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Source files
file(GLOB SOURCES "src/*.cpp")

# Create executable
add_executable(http_server ${SOURCES})

# Link Winsock2 library on Windows
if(WIN32)
    target_link_libraries(http_server ws2_32)
endif()

# Create www directory if it doesn't exist
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/www)

# Copy www directory to build directory
add_custom_command(TARGET http_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/www
        $<TARGET_FILE_DIR:http_server>/www
)