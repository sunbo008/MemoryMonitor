cmake_minimum_required(VERSION 3.15)
project(MemoryMoniter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MemoryMoniter WIN32 main.cpp)

# Force Unicode build
target_compile_definitions(MemoryMoniter PRIVATE UNICODE _UNICODE)

# Link required Windows libraries
target_link_libraries(MemoryMoniter PRIVATE Psapi user32 gdi32)
