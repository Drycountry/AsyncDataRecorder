# 单元测试，基于 GoogleTest，源码随工程一同构建
# 关闭方式：cmake -DBUILD_TESTING=OFF ...
# GoogleTest 优先使用已安装的版本；找不到时才联网下载，避免离线环境无法配置

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)

  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
  )

  # 与主工程使用一致的运行库（MSVC 下 /MD），避免混用 CRT；测试不涉及 GMock
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(googletest)
endif()

add_executable(recorder_unit_tests
  test_helpers.hpp
  test_helpers.cpp
  test_recorder.cpp
  test_recorder_manager.cpp
  ${RECORDER_SOURCES}
)

target_include_directories(recorder_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(recorder_unit_tests PRIVATE flatbuffers GTest::gtest_main)

# 每个 TEST 注册为一条 CTest 用例，便于筛选与定位失败
include(GoogleTest)
gtest_discover_tests(recorder_unit_tests)
