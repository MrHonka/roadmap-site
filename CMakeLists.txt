cmake_minimum_required(VERSION 3.16)
project(roadmap_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(roadmap_server src/main.cpp)

target_include_directories(roadmap_server PRIVATE src)
target_link_libraries(roadmap_server PRIVATE Threads::Threads)

if(WIN32)
  target_link_libraries(roadmap_server PRIVATE ws2_32)
endif()
