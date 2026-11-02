#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "recorder_manager.hpp"
#include "test_helpers.hpp"

namespace jojo::record2::test {
namespace {

/// @brief RecorderManager 以 id 作为基础文件名，扩展名固定为 .dat
constexpr char kExt[] = ".dat";

}  // namespace

/// @brief 同一 id 复用同一实例，不同 id 相互隔离
TEST_F(TempWorkDir, GetRecorderCachesByID) {
  RecorderManager manager;

  const auto first = manager.GetRecorder("1");
  const auto other = manager.GetRecorder("2");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(first, manager.GetRecorder("1"));
  EXPECT_NE(first, other);
}

/// @brief 并发获取同一 id 只能得到同一实例
TEST_F(TempWorkDir, GetRecorderIsThreadSafe) {
  constexpr int kThreads = 8;
  constexpr int kCallsPerThread = 200;

  RecorderManager manager;
  const auto reference = manager.GetRecorder("shared");

  std::atomic<int> mismatched{};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i{}; i < kThreads; ++i) {
    threads.emplace_back([&manager, &reference, &mismatched, kCallsPerThread]() {
      for (int j{}; j < kCallsPerThread; ++j) {
        if (manager.GetRecorder("shared") != reference) {
          mismatched.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(mismatched.load(), 0);
}

/// @brief 记录器由管理器释放，释放前须把数据落盘并定型文件名
TEST_F(TempWorkDir, GetRecorderFlushesDataWhenManagerDestroyed) {
  const std::vector<std::uint8_t> data{1, 2, 3, 4, 5};
  {
    RecorderManager manager;
    const auto recorder = manager.GetRecorder("1");
    ASSERT_NE(recorder, nullptr);
    ASSERT_TRUE(recorder->Save(data.data(), data.size()));
  }

  const auto finalized = FindFinalizedFiles(dir(), "1", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), data));
}

/// @brief 始终无数据的记录器不留文件
TEST_F(TempWorkDir, GetRecorderLeavesNoFileWithoutData) {
  {
    RecorderManager manager;
    ASSERT_NE(manager.GetRecorder("1"), nullptr);
  }

  EXPECT_TRUE(std::filesystem::is_empty(dir()));
}

/// @brief 不同 id 的记录器各自落盘到独立文件
TEST_F(TempWorkDir, GetRecorderDistinctIDsWriteSeparateFiles) {
  const std::vector<std::uint8_t> data_a{1, 1};
  const std::vector<std::uint8_t> data_b{2};

  {
    RecorderManager manager;
    const auto recorder_a = manager.GetRecorder("a");
    const auto recorder_b = manager.GetRecorder("b");
    ASSERT_NE(recorder_a, nullptr);
    ASSERT_NE(recorder_b, nullptr);
    EXPECT_NE(recorder_a, recorder_b);
    ASSERT_TRUE(recorder_a->Save(data_a.data(), data_a.size()));
    ASSERT_TRUE(recorder_b->Save(data_b.data(), data_b.size()));
  }

  const auto finalized_a = FindFinalizedFiles(dir(), "a", kExt);
  const auto finalized_b = FindFinalizedFiles(dir(), "b", kExt);
  ASSERT_EQ(finalized_a.size(), 1u);
  ASSERT_EQ(finalized_b.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized_a.front()), data_a));
  EXPECT_TRUE(BytesEqual(ReadFile(finalized_b.front()), data_b));
}

}  // namespace jojo::record2::test
