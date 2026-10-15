cmake_minimum_required(VERSION 3.20)
project("AsyncDataRecorder")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  if(MSVC)
    add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  endif()
endif()

add_executable(${PROJECT_NAME}
  main.cpp
  recorder.hpp
  recorder.cpp
  recorder_manager.hpp
  recorder_manager.cpp
  fixed_buffer.hpp
  utils.hpp
  utils.cpp
)