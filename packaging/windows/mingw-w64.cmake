# WSL から Windows x64（MinGW-w64）向けに組むための CMake ツールチェーン。
# 例:
#   cmake -S . -B build-windows -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=packaging/windows/mingw-w64.cmake \
#     -DCMAKE_PREFIX_PATH="$HOME/Qt/6.2.4/mingw_64" \
#     -DQT_HOST_PATH=/usr/lib/x86_64-linux-gnu/qt6
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
endif()
if(NOT CMAKE_RC_COMPILER)
    set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
endif()

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Qt は CMAKE_PREFIX_PATH（WSL の Qt for Windows）から拾う
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
