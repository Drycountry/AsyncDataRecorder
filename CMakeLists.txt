cmake_minimum_required(VERSION 3.20)
project("AsyncBinaryRecorder")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8)
endif()

set(RECORDER_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/recorder.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/recorder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/recorder_manager.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/recorder_manager.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/fixed_buffer.hpp"
)

add_executable(${PROJECT_NAME}
  main.cpp
  ${RECORDER_SOURCES}
)
# 可执行文件不会被其他目标链接，包含目录只需 PRIVATE
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
