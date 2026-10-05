# mcsr3 link: dinput8.dll proxy loaded by SRTTR.exe. Talks SkyCraft's shared-memory protocol.
cmake_minimum_required(VERSION 3.24)
project(mcsr3_link CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: nothing extra to install next to the game

include(FetchContent)
FetchContent_Declare(minhook
	GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
	GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)  # v1.3.4
FetchContent_MakeAvailable(minhook)

set(SKYCRAFT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../skycraft" CACHE PATH "SkyCraft checkout (tools/fetch_sources.sh)")

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(dinput8 SHARED ${SOURCES})
target_include_directories(dinput8 PRIVATE src "${SKYCRAFT_DIR}/protocol")
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dinput8 PRIVATE /W4 /wd4100 /utf-8 /Zi)
target_link_options(dinput8 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
target_link_libraries(dinput8 PRIVATE minhook d3d11 dxgi d3dcompiler)
