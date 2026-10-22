cmake_minimum_required(VERSION 3.16)

project(sgnano_remote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_executable(sgnano_remote
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/localpane.cpp
    src/localpane.h
    src/remotepane.cpp
    src/remotepane.h
    src/remoteprotocol.cpp
    src/remoteprotocol.h
    src/seriallink.cpp
    src/seriallink.h
    src/transfermime.h
)

target_link_libraries(sgnano_remote PRIVATE Qt6::Widgets)
target_include_directories(sgnano_remote PRIVATE src)
target_compile_options(sgnano_remote PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS sgnano_remote RUNTIME DESTINATION bin)
