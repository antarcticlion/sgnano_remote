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
    src/remotedeletequeue.cpp
    src/remotedeletequeue.h
    src/remotefs.cpp
    src/remotefs.h
    src/remoteprotocol.cpp
    src/remoteprotocol.h
    src/seriallink.cpp
    src/seriallink.h
    src/transfermime.h
    src/uploadsplit.cpp
    src/uploadsplit.h
    src/zmodem.cpp
    src/zmodem.h
    src/zmodemworker.cpp
    src/zmodemworker.h
)

target_link_libraries(sgnano_remote PRIVATE Qt6::Widgets)
target_include_directories(sgnano_remote PRIVATE src)
target_compile_options(sgnano_remote PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS sgnano_remote RUNTIME DESTINATION bin)

include(CTest)

# 試し（2026-10-23）: 実機なしでプロトコルの口を、--device で実機の DEL / REN / MKDIR を確かめる
if(BUILD_TESTING)
    add_executable(sgnano_remote_selftest
        tests/protocol_selftest.cpp
        src/remotedeletequeue.cpp
        src/remotedeletequeue.h
        src/remotefs.cpp
        src/remotefs.h
        src/remoteprotocol.cpp
        src/remoteprotocol.h
        src/seriallink.cpp
        src/seriallink.h
        src/uploadsplit.cpp
        src/uploadsplit.h
        src/zmodem.cpp
        src/zmodem.h
        src/zmodemworker.cpp
        src/zmodemworker.h
    )
    target_link_libraries(sgnano_remote_selftest PRIVATE Qt6::Core)
    target_include_directories(sgnano_remote_selftest PRIVATE src)
    target_compile_options(sgnano_remote_selftest PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME sgnano_remote_selftest COMMAND sgnano_remote_selftest)
    set_tests_properties(sgnano_remote_selftest PROPERTIES TIMEOUT 30)

    add_executable(sgnano_remote_zmodem_selftest
        tests/zmodem_selftest.cpp
        src/zmodem.cpp
        src/zmodem.h
    )
    target_link_libraries(sgnano_remote_zmodem_selftest PRIVATE Qt6::Core)
    target_include_directories(sgnano_remote_zmodem_selftest PRIVATE src)
    target_compile_options(sgnano_remote_zmodem_selftest PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME sgnano_remote_zmodem_selftest COMMAND sgnano_remote_zmodem_selftest)
    set_tests_properties(sgnano_remote_zmodem_selftest PROPERTIES TIMEOUT 120)

    # 試し（2026-11-14）: 4 MB を越えるファイルを name.001 .. に分ける（本体の JOIN は 64 個まで）
    add_executable(sgnano_remote_uploadsplit_selftest
        tests/uploadsplit_selftest.cpp
        src/uploadsplit.cpp
        src/uploadsplit.h
    )
    target_link_libraries(sgnano_remote_uploadsplit_selftest PRIVATE Qt6::Core)
    target_include_directories(sgnano_remote_uploadsplit_selftest PRIVATE src)
    target_compile_options(sgnano_remote_uploadsplit_selftest PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME sgnano_remote_uploadsplit_selftest COMMAND sgnano_remote_uploadsplit_selftest)
    set_tests_properties(sgnano_remote_uploadsplit_selftest PROPERTIES TIMEOUT 60)
endif()
