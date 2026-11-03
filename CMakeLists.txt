cmake_minimum_required(VERSION 3.20)
project("AsyncDataRecorder")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  if(MSVC)
    add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  endif()
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

# 第三方库：FlatBuffers
set(FLATBUFFERS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/flatbuffers")
add_library(flatbuffers INTERFACE)
target_include_directories(flatbuffers SYSTEM INTERFACE "${FLATBUFFERS_ROOT}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE flatbuffers)

# BUILD_TESTING 默认为 ON，可用 -DBUILD_TESTING=OFF 关闭单元测试
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(test)
endif()