#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "recorder.hpp"
#include "test_helpers.hpp"

namespace jojo::record2::test {
namespace {

/// @brief 单个缓存的容量，也是触发缓存轮转的数据量
constexpr std::size_t kChunkSize = kBufferSize;

/// @brief 测试用扩展名
constexpr char kExt[] = ".dat";

/// @brief 时间戳字符串长度，yyyyMMddTHHmmss.fff
constexpr std::size_t kTimestampLength = 19;

/// @brief 判断字符串是否为 yyyyMMddTHHmmss.fff 形式的时间戳
bool IsTimestamp(const std::string& text) {
  if (text.size() != kTimestampLength) {
    return false;
  }
  for (std::size_t i{}; i < text.size(); ++i) {
    if (i == 8) {
      if (text[i] != 'T') {
        return false;
      }
      continue;
    }
    if (i == 15) {
      if (text[i] != '.') {
        return false;
      }
      continue;
    }
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
  }
  return true;
}

/// @brief 拆解定型文件名：基础名_建立时间戳_关闭时间戳 + 扩展名
bool ParseFinalizedName(const std::string& name, const std::string& basic_filename,
                        std::string& create_time, std::string& close_time) {
  const std::string prefix = basic_filename + "_";
  const std::string suffix{kExt};
  if (name.size() != prefix.size() + kTimestampLength * 2 + 1 + suffix.size() ||
      name.compare(0, prefix.size(), prefix) != 0 ||
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }

  const std::string times = name.substr(prefix.size(), kTimestampLength * 2 + 1);
  if (times[kTimestampLength] != '_') {
    return false;
  }
  create_time = times.substr(0, kTimestampLength);
  close_time = times.substr(kTimestampLength + 1);
  return IsTimestamp(create_time) && IsTimestamp(close_time);
}

/// @brief 把 yyyyMMddTHHmmss.fff 换算为毫秒数，仅用于比较两枚时间戳的间隔
std::int64_t TimestampToMillis(const std::string& timestamp) {
  std::tm tm{};
  tm.tm_year = std::stoi(timestamp.substr(0, 4)) - 1900;
  tm.tm_mon = std::stoi(timestamp.substr(4, 2)) - 1;
  tm.tm_mday = std::stoi(timestamp.substr(6, 2));
  tm.tm_hour = std::stoi(timestamp.substr(9, 2));
  tm.tm_min = std::stoi(timestamp.substr(11, 2));
  tm.tm_sec = std::stoi(timestamp.substr(13, 2));
  tm.tm_isdst = -1;  // 夏令时由 mktime 自行判断
  const auto seconds = static_cast<std::int64_t>(std::mktime(&tm));
  return seconds * 1000 + std::stoi(timestamp.substr(16, 3));
}

/// @brief 生成 chunks 个定长块首尾相接的数据，第 i 个块的字节值均为 i+1
std::vector<std::uint8_t> ExpectedChunks(std::size_t chunks, std::size_t chunk_size) {
  std::vector<std::uint8_t> out;
  out.reserve(chunks * chunk_size);
  for (std::size_t i{}; i < chunks; ++i) {
    const auto block = MakeBlock(chunk_size, static_cast<std::uint8_t>(i + 1));
    out.insert(out.end(), block.cbegin(), block.cend());
  }
  return out;
}

/// @brief 轮询等待写线程状态达到期望值
bool WaitForWriteState(const Recorder& recorder, WriteIoState expect, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (recorder.GetWriteState() != expect && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return recorder.GetWriteState() == expect;
}

}  // namespace

/// @brief 析构时必须把未满缓存中的残留数据一并落盘，并把过渡文件定型
TEST_F(TempWorkDir, SaveFlushesDataOnDestruction) {
  const std::vector<std::uint8_t> data{1, 2, 3, 4, 5};
  {
    Recorder recorder{"rec_flush", kExt};
    EXPECT_TRUE(recorder.Save(data.data(), data.size()));
  }

  const auto finalized = FindFinalizedFiles(dir(), "rec_flush", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(FindTransitionalFiles(dir(), "rec_flush", kExt).empty()) << "过渡文件应已定型";
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), data));
}

/// @brief 正常运行时不应记录错误原因
TEST_F(TempWorkDir, GetLastErrorIsNullWhenHealthy) {
  const std::vector<std::uint8_t> data{1, 2, 3};
  {
    Recorder recorder{"rec_healthy", kExt};
    EXPECT_EQ(recorder.GetLastError(), nullptr);
    ASSERT_TRUE(recorder.Save(data.data(), data.size()));
    EXPECT_EQ(recorder.GetLastError(), nullptr);
  }

  EXPECT_TRUE(BytesEqual(ReadFile(FindFinalizedFiles(dir(), "rec_healthy", kExt).front()), data));
}

/// @brief 定型文件名须为"基础名_建立时间戳_关闭时间戳 + 扩展名"
TEST_F(TempWorkDir, FinalizedNameCarriesCreateAndCloseTime) {
  const std::vector<std::uint8_t> data{1, 2, 3};
  {
    Recorder recorder{"rec_name", kExt};
    ASSERT_TRUE(recorder.Save(data.data(), data.size()));
  }

  const auto finalized = FindFinalizedFiles(dir(), "rec_name", kExt);
  ASSERT_EQ(finalized.size(), 1u);

  const std::string name = finalized.front().filename().string();
  std::string create_time;
  std::string close_time;
  ASSERT_TRUE(ParseFinalizedName(name, "rec_name", create_time, close_time)) << "定型文件名不符：" << name;
  EXPECT_LE(create_time, close_time) << "建立时间戳不应晚于关闭时间戳：" << name;
}

/// @brief 时间戳的毫秒位须反映真实经过的时间，而非固定为 0
TEST_F(TempWorkDir, FinalizedTimestampsCarryMilliseconds) {
  const std::vector<std::uint8_t> data{1, 2, 3};
  const auto begin = std::chrono::steady_clock::now();
  {
    Recorder recorder{"rec_ms", kExt};
    ASSERT_TRUE(recorder.Save(data.data(), data.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));  // 让关闭时间戳与建立时间戳拉开差距
  }  // 析构时取关闭时间戳
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

  const auto finalized = FindFinalizedFiles(dir(), "rec_ms", kExt);
  ASSERT_EQ(finalized.size(), 1u);

  const std::string name = finalized.front().filename().string();
  std::string create_time;
  std::string close_time;
  ASSERT_TRUE(ParseFinalizedName(name, "rec_ms", create_time, close_time)) << "定型文件名不符：" << name;

  const auto span = TimestampToMillis(close_time) - TimestampToMillis(create_time);
  EXPECT_GE(span, 100) << "毫秒位可能未生效：" << create_time << " -> " << close_time;
  EXPECT_LE(span, elapsed + 100) << "时间戳间隔超出实际耗时：" << create_time << " -> " << close_time;
}

/// @brief 跨越多级缓存的数据必须按写入顺序完整落盘
TEST_F(TempWorkDir, SaveAcrossBuffersKeepsOrder) {
  constexpr std::size_t kChunks = 3;
  const auto expected = ExpectedChunks(kChunks, kChunkSize);

  {
    Recorder recorder{"rec_order", kExt};
    for (std::size_t i{}; i < kChunks; ++i) {
      const auto block = MakeBlock(kChunkSize, static_cast<std::uint8_t>(i + 1));
      ASSERT_TRUE(recorder.Save(block.data(), block.size()));
    }
  }

  const auto finalized = FindFinalizedFiles(dir(), "rec_order", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), expected));
}

/// @brief 非法入参（空指针、零长度）不被接收
TEST_F(TempWorkDir, SaveRejectsInvalidArguments) {
  const std::uint8_t byte{0x5A};
  {
    Recorder recorder{"rec_invalid", kExt};
    EXPECT_FALSE(recorder.Save(nullptr, 1));
    EXPECT_FALSE(recorder.Save(&byte, 0));
    EXPECT_TRUE(recorder.Save(&byte, 1));
  }

  const auto finalized = FindFinalizedFiles(dir(), "rec_invalid", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), std::vector<std::uint8_t>{byte}));
}

/// @brief 超过单个缓存容量的数据被拒收，不影响同一实例后续落盘
TEST_F(TempWorkDir, SaveRejectsOversizeData) {
  const auto oversize = MakeBlock(kChunkSize + 1, 0xAB);
  const std::vector<std::uint8_t> valid{9, 8, 7};

  {
    Recorder recorder{"rec_oversize", kExt};
    EXPECT_FALSE(recorder.Save(oversize.data(), oversize.size()));
    EXPECT_TRUE(recorder.Save(valid.data(), valid.size()));
  }

  const auto finalized = FindFinalizedFiles(dir(), "rec_oversize", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), valid));
}

/// @brief 满缓存达到阈值时由写线程提前落盘，无需等待析构
TEST_F(TempWorkDir, SaveIsWrittenBeforeDestruction) {
  constexpr std::size_t kChunks = 6;
  // 等待上限取 3s，小于写线程内部的定时落盘周期（5s），
  // 故测试通过即说明落盘由满缓存阈值触发，而非定时或析构触发
  constexpr auto kNotifyFlushTimeout = std::chrono::seconds(3);
  const auto expected = ExpectedChunks(kChunks, kChunkSize);

  {
    Recorder recorder{"rec_async", kExt};
    for (std::size_t i{}; i < kChunks; ++i) {
      const auto block = MakeBlock(kChunkSize, static_cast<std::uint8_t>(i + 1));
      ASSERT_TRUE(recorder.Save(block.data(), block.size()));
    }

    const auto transitional = WaitForTransitionalFile(dir(), "rec_async", kExt, kNotifyFlushTimeout);
    ASSERT_FALSE(transitional.empty()) << "追加中的过渡文件应已建立";
    ASSERT_TRUE(WaitForFileSizeAtLeast(transitional, kChunkSize, kNotifyFlushTimeout))
        << "等待写线程提前落盘超时：" << transitional.string();

    // 已落盘的部分必须是完整数据的前缀：块值随块序号递增，据此校验顺序与完整性
    const auto partial = ReadFile(transitional);
    ASSERT_GE(partial.size(), kChunkSize);
    for (std::size_t offset{}; offset < partial.size(); ++offset) {
      ASSERT_EQ(partial[offset], static_cast<std::uint8_t>(offset / kChunkSize + 1))
          << "第 " << offset << " 字节与预期不符";
    }
  }  // 析构补齐缓存中的剩余数据

  const auto finalized = FindFinalizedFiles(dir(), "rec_async", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized.front()), expected));
}

/// @brief 始终无数据的记录器不留文件
TEST_F(TempWorkDir, EmptyRecorderLeavesNoFile) {
  { Recorder recorder{"rec_empty", kExt}; }

  EXPECT_TRUE(std::filesystem::is_empty(dir())) << "空段不应留下文件";
}

/// @brief 落盘无法进行且重试达上限时写线程进入 kFatal，此后不再接收数据
TEST_F(TempWorkDir, WriteStateBecomesFatalWhenFileUnopenable) {
  const std::vector<std::uint8_t> data{1, 2, 3};
  {
    // 指向不存在的子目录，文件必定无法创建
    Recorder recorder{"no_such_dir/rec_fatal", kExt};
    ASSERT_TRUE(recorder.Save(data.data(), data.size()));

    ASSERT_TRUE(WaitForWriteState(recorder, WriteIoState::kFatal, std::chrono::seconds(5)))
        << "写线程未在重试上限后放弃落盘";

    // 停止接收的原因应可查询
    const char* reason = recorder.GetLastError();
    ASSERT_NE(reason, nullptr) << "进入 kFatal 时应记录错误原因";
    EXPECT_NE(std::string{reason}.find("文件打开失败"), std::string::npos) << "错误原因不符：" << reason;

    // 进入 kFatal 后一律拒收后续数据
    EXPECT_FALSE(recorder.Save(data.data(), data.size()));
  }

  EXPECT_TRUE(std::filesystem::is_empty(dir())) << "无法写入时不应留下文件";
}

/// @brief 多线程并发写入时不得丢失、交错或拒收数据
TEST_F(TempWorkDir, ConcurrentSaveKeepsDataIntact) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kWritesPerThread = 500;
  constexpr std::size_t kRecordSize = 100;
  constexpr std::size_t kTotalRecords = kThreads * kWritesPerThread;

  std::atomic<std::size_t> rejected{};
  {
    Recorder recorder{"rec_concurrent", kExt};

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i{}; i < kThreads; ++i) {
      threads.emplace_back([&recorder, &start, &rejected, i, kRecordSize, kWritesPerThread]() {
        // 每条记录为同值字节组成的定长块，块首字节即线程编号，便于校验是否交错
        const auto block = MakeBlock(kRecordSize, static_cast<std::uint8_t>(i + 1));
        while (start.load(std::memory_order_acquire) == false) {
          std::this_thread::yield();
        }
        for (std::size_t j{}; j < kWritesPerThread; ++j) {
          if (recorder.Save(block.data(), block.size()) == false) {
            rejected.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
      thread.join();
    }
  }

  EXPECT_EQ(rejected.load(), 0u);

  const auto finalized = FindFinalizedFiles(dir(), "rec_concurrent", kExt);
  ASSERT_EQ(finalized.size(), 1u);
  const auto bytes = ReadFile(finalized.front());
  ASSERT_EQ(bytes.size(), kTotalRecords * kRecordSize);

  std::vector<std::size_t> records_per_thread(kThreads + 1);
  std::size_t broken_records{};
  for (std::size_t offset{}; offset < bytes.size(); offset += kRecordSize) {
    const std::uint8_t value = bytes[offset];
    const auto begin = bytes.cbegin() + static_cast<std::ptrdiff_t>(offset);
    if (value < 1 || value > kThreads ||
        std::count(begin, begin + static_cast<std::ptrdiff_t>(kRecordSize), value) !=
            static_cast<std::ptrdiff_t>(kRecordSize)) {
      ++broken_records;
      continue;
    }
    ++records_per_thread[value];
  }

  EXPECT_EQ(broken_records, 0u) << "存在被撕裂或越界的记录，并发写入发生交错";
  for (std::size_t i{1}; i <= kThreads; ++i) {
    EXPECT_EQ(records_per_thread[i], kWritesPerThread) << "线程 " << i << " 的记录数不符";
  }
}

/// @brief 不同基础文件名各自落盘到独立文件，互不干扰
TEST_F(TempWorkDir, DistinctBasicFilenameWritesDistinctFile) {
  const std::vector<std::uint8_t> data_a{1, 1, 1};
  const std::vector<std::uint8_t> data_b{2, 2};

  {
    Recorder recorder_a{"rec_a", kExt};
    Recorder recorder_b{"rec_b", kExt};
    ASSERT_TRUE(recorder_a.Save(data_a.data(), data_a.size()));
    ASSERT_TRUE(recorder_b.Save(data_b.data(), data_b.size()));
  }

  const auto finalized_a = FindFinalizedFiles(dir(), "rec_a", kExt);
  const auto finalized_b = FindFinalizedFiles(dir(), "rec_b", kExt);
  ASSERT_EQ(finalized_a.size(), 1u);
  ASSERT_EQ(finalized_b.size(), 1u);
  EXPECT_TRUE(BytesEqual(ReadFile(finalized_a.front()), data_a));
  EXPECT_TRUE(BytesEqual(ReadFile(finalized_b.front()), data_b));
}

}  // namespace jojo::record2::test
