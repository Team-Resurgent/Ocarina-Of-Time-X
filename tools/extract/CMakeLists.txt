# Ocarina-Of-Time-X: Ship of Harkinian's two PC tools that make the game's archives, built on their own (not the whole
# PC game); setup.py configures and builds this with Visual Studio, then runs them:
#   soh-torch       your ROM -> data/oot.o2r (oot-mq.o2r for a Master Quest ROM)
#   soh-o2r-packer  SoH's own extra files (soh/assets/custom) -> data/soh.o2r
# The same targets and Torch options as Shipwright/CMakeLists.txt. Their sources are this repo's (Shipwright/soh/assets/
# tools, Shipwright/soh/soh/Extractor); Torch itself is fetched by setup.py at the commit Ship of Harkinian 9.3.1 pins,
# and passed in as TORCH_DIR.
cmake_minimum_required(VERSION 3.26)
project(ocarina-of-time-x-extract LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(SHIP ${CMAKE_CURRENT_SOURCE_DIR}/../../Shipwright)
if(NOT TORCH_DIR)
    message(FATAL_ERROR "TORCH_DIR not set: run setup.py, which fetches Torch and passes it in")
endif()

include(FetchContent)
# as Shipwright: declared before torch, so the first declaration is this one
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.16.0 OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(spdlog)

set(USE_STANDALONE          OFF CACHE BOOL "" FORCE)
set(PORT_VERSION_ENDIANNESS ON  CACHE BOOL "" FORCE)
set(ROM_CRC_BSWAP           OFF CACHE BOOL "" FORCE)
set(BUILD_UI                OFF CACHE BOOL "" FORCE)
set(BUILD_STORMLIB          OFF CACHE BOOL "" FORCE)
set(BUILD_NAUDIO            OFF CACHE BOOL "" FORCE)
set(BUILD_OOT               ON  CACHE BOOL "" FORCE)
foreach(_torch_game SM64 MK64 SF64 PM64 FZERO BK64 MARIO_ARTIST)
    set(BUILD_${_torch_game} OFF CACHE BOOL "" FORCE)
endforeach()

add_subdirectory(${TORCH_DIR} ${CMAKE_BINARY_DIR}/torch SYSTEM)
target_compile_definitions(torch INTERFACE YAML_CPP_STATIC_DEFINE)

add_executable(soh-torch
    ${SHIP}/soh/assets/tools/torch-cli/main.cpp
    ${SHIP}/soh/soh/Extractor/TorchExtract.cpp
)
target_include_directories(soh-torch PRIVATE ${SHIP}/soh/soh/Extractor)
target_link_libraries(soh-torch PRIVATE torch)

add_executable(soh-o2r-packer
    ${SHIP}/soh/assets/tools/soh-o2r-packer/main.cpp
    ${SHIP}/soh/assets/tools/soh-o2r-packer/PngTexture.cpp
)
target_link_libraries(soh-o2r-packer PRIVATE torch)
