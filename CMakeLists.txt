cmake_minimum_required(VERSION 3.16)

project(sgnano_remote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

set(SGNANO_REMOTE_SOURCES
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
    resources/resources.qrc
)

if(WIN32)
    list(APPEND SGNANO_REMOTE_SOURCES
        src/win32serial.cpp
        src/win32serial.h
    )
    # Windows の exe にアイコンを埋める（エクスプローラ・タスクバー用）
    set(SGNANO_REMOTE_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/sgnano_remote_icon.rc")
    file(WRITE "${SGNANO_REMOTE_ICON_RC}"
         "IDI_ICON1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/sgnano_remote.ico\"\n")
    list(APPEND SGNANO_REMOTE_SOURCES "${SGNANO_REMOTE_ICON_RC}")
endif()

if(WIN32)
    add_executable(sgnano_remote WIN32 ${SGNANO_REMOTE_SOURCES})
else()
    add_executable(sgnano_remote ${SGNANO_REMOTE_SOURCES})
endif()

target_link_libraries(sgnano_remote PRIVATE Qt6::Widgets)
target_include_directories(sgnano_remote PRIVATE src)
if(MSVC)
    target_compile_options(sgnano_remote PRIVATE /W4)
else()
    target_compile_options(sgnano_remote PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        # Qt の Windows DLL は GCC 11.2 で組まれている。こちらは WSL の MinGW なので、
        # アプリ側の libstdc++/libgcc は静的にして、Qt の実行時 DLL と混ぜない
        target_link_options(sgnano_remote PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

if(WIN32)
    install(TARGETS sgnano_remote RUNTIME DESTINATION .)
    install(FILES README.md LICENSE DESTINATION . OPTIONAL)
else()
    install(TARGETS sgnano_remote RUNTIME DESTINATION bin)
    install(FILES README.md LICENSE DESTINATION share/doc/sgnano_remote OPTIONAL)
endif()

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
    if(WIN32)
        target_sources(sgnano_remote_selftest PRIVATE src/win32serial.cpp src/win32serial.h)
    endif()
    target_link_libraries(sgnano_remote_selftest PRIVATE Qt6::Core)
    target_include_directories(sgnano_remote_selftest PRIVATE src)
    if(MSVC)
        target_compile_options(sgnano_remote_selftest PRIVATE /W4)
    else()
        target_compile_options(sgnano_remote_selftest PRIVATE -Wall -Wextra -Wpedantic)
        if(WIN32)
            target_link_options(sgnano_remote_selftest PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endif()

    add_test(NAME sgnano_remote_selftest COMMAND sgnano_remote_selftest)
    set_tests_properties(sgnano_remote_selftest PROPERTIES TIMEOUT 30)

    # socketpair / poll を使うので、この試しは Linux（WSL）だけで組む
    if(NOT WIN32)
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
    endif()

    # 試し（2026-11-14）: 4 MB を越えるファイルを name.001 .. に分ける（本体の JOIN は 64 個まで）
    add_executable(sgnano_remote_uploadsplit_selftest
        tests/uploadsplit_selftest.cpp
        src/uploadsplit.cpp
        src/uploadsplit.h
    )
    target_link_libraries(sgnano_remote_uploadsplit_selftest PRIVATE Qt6::Core)
    target_include_directories(sgnano_remote_uploadsplit_selftest PRIVATE src)
    if(MSVC)
        target_compile_options(sgnano_remote_uploadsplit_selftest PRIVATE /W4)
    else()
        target_compile_options(sgnano_remote_uploadsplit_selftest PRIVATE -Wall -Wextra -Wpedantic)
        if(WIN32)
            target_link_options(sgnano_remote_uploadsplit_selftest PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endif()

    add_test(NAME sgnano_remote_uploadsplit_selftest COMMAND sgnano_remote_uploadsplit_selftest)
    set_tests_properties(sgnano_remote_uploadsplit_selftest PROPERTIES TIMEOUT 60)
endif()
