cmake_minimum_required(VERSION 3.16)

project(voice_changer LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 明确不做架构相关优化，保证 x86_64 与 aarch64 行为一致
# （不使用 -march=native / -mavx / -mfpu，也不引入任何手写 SIMD）

add_library(vc_core STATIC
  src/fft.cpp
  src/resampler.cpp
  src/phase_vocoder.cpp
  src/pitch_shifter.cpp
  src/wav_io.cpp
)

target_include_directories(vc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(vc_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(voice_changer src/main.cpp)
target_link_libraries(voice_changer PRIVATE vc_core)

option(VC_BUILD_TESTS "Build unit tests" ON)

if(VC_BUILD_TESTS)
  enable_testing()
  add_executable(vc_tests tests/test_main.cpp)
  target_link_libraries(vc_tests PRIVATE vc_core)
  add_test(NAME vc_tests COMMAND vc_tests)
endif()

# ---- 交叉编译到 Zynq PS 端（aarch64 Linux 用户态）----
# 示例用法：
#   cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
# 工具链文件内容见 README 的「交叉编译」一节。
if(CMAKE_CROSSCOMPILING)
  message(STATUS "Cross compiling: ${CMAKE_SYSTEM_PROCESSOR} / ${CMAKE_CXX_COMPILER}")
endif()

install(TARGETS voice_changer RUNTIME DESTINATION bin)
