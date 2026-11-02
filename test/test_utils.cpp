#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "utils.hpp"

namespace jojo::record2::test {
namespace {

/// @brief 时间戳长度，yyyyMMddTHHmmss.fff 共 19 字符
constexpr std::size_t kTimestampLength = 19;

/// @brief 解析后的时间戳
struct ParsedTimestamp {
  std::tm tm{};         // 本地时间（秒以上部分）
  int millis{};         // 毫秒部分
  std::time_t epoch{};  // 按本地时间换算的 Unix 时间戳
};

/// @brief 严格解析形如 20261015T104827.321 的时间戳，格式或数值越界时返回 false
bool ParseTimestamp(const std::string& timestamp, ParsedTimestamp& out) {
  if (timestamp.size() != kTimestampLength) {
    return false;
  }
  for (std::size_t i{}; i < timestamp.size(); ++i) {
    if (i == 8) {
      if (timestamp[i] != 'T') {
        return false;
      }
      continue;
    }
    if (i == 15) {
      if (timestamp[i] != '.') {
        return false;
      }
      continue;
    }
    if (timestamp[i] < '0' || timestamp[i] > '9') {
      return false;
    }
  }

  out.tm.tm_year = std::stoi(timestamp.substr(0, 4)) - 1900;
  out.tm.tm_mon = std::stoi(timestamp.substr(4, 2)) - 1;
  out.tm.tm_mday = std::stoi(timestamp.substr(6, 2));
  out.tm.tm_hour = std::stoi(timestamp.substr(9, 2));
  out.tm.tm_min = std::stoi(timestamp.substr(11, 2));
  out.tm.tm_sec = std::stoi(timestamp.substr(13, 2));
  out.millis = std::stoi(timestamp.substr(16, 3));

  const bool in_range = (out.tm.tm_year >= 70) && (out.tm.tm_mon >= 0) && (out.tm.tm_mon <= 11) &&
                        (out.tm.tm_mday >= 1) && (out.tm.tm_mday <= 31) && (out.tm.tm_hour >= 0) &&
                        (out.tm.tm_hour <= 23) && (out.tm.tm_min >= 0) && (out.tm.tm_min <= 59) &&
                        (out.tm.tm_sec >= 0) && (out.tm.tm_sec <= 60) && (out.millis >= 0) &&
                        (out.millis <= 999);
  if (in_range == false) {
    return false;
  }

  out.tm.tm_isdst = -1;  // 夏令时由 mktime 自行判断
  out.epoch = std::mktime(&out.tm);
  return out.epoch != static_cast<std::time_t>(-1);
}

/// @brief 时间戳的毫秒表示，用于比较两个时间戳的间隔
std::int64_t ToMillis(const ParsedTimestamp& parsed) {
  return static_cast<std::int64_t>(parsed.epoch) * 1000 + parsed.millis;
}

/// @brief 采集一段时间内的多个时间戳，覆盖毫秒位变化的场景
std::vector<std::string> Sample(int count) {
  std::vector<std::string> samples;
  samples.reserve(static_cast<std::size_t>(count));
  for (int i{}; i < count; ++i) {
    samples.push_back(Utils::GetTimestamp());
  }
  return samples;
}

}  // namespace

/// @brief 格式必须为 yyyyMMddTHHmmss.fff，长度固定 19 字符
TEST(UtilsTest, GetTimestampFormat) {
  const std::string timestamp = Utils::GetTimestamp();
  ParsedTimestamp parsed;

  EXPECT_TRUE(ParseTimestamp(timestamp, parsed)) << "时间戳格式不符：" << timestamp;
  EXPECT_EQ(timestamp.size(), kTimestampLength);
  EXPECT_EQ(timestamp[8], 'T');
  EXPECT_EQ(timestamp[15], '.');
}

/// @brief 时间戳须为本地时间，且与系统当前时间一致
TEST(UtilsTest, GetTimestampMatchesLocalTime) {
  const auto before = std::chrono::system_clock::now();
  const std::string timestamp = Utils::GetTimestamp();
  const auto after = std::chrono::system_clock::now();

  ParsedTimestamp parsed;
  ASSERT_TRUE(ParseTimestamp(timestamp, parsed)) << "时间戳格式不符：" << timestamp;

  const auto before_seconds = std::chrono::system_clock::to_time_t(before);
  const auto after_seconds = std::chrono::system_clock::to_time_t(after);
  EXPECT_GE(parsed.epoch, before_seconds - 1) << "时间戳早于调用时刻：" << timestamp;
  EXPECT_LE(parsed.epoch, after_seconds + 1) << "时间戳晚于调用时刻：" << timestamp;
}

/// @brief 定长零填充的时间戳，字典序与时间先后一致，据此校验单调性
TEST(UtilsTest, GetTimestampIsMonotonic) {
  const auto samples = Sample(1000);
  for (std::size_t i{}; i < samples.size(); ++i) {
    ParsedTimestamp parsed;
    ASSERT_TRUE(ParseTimestamp(samples[i], parsed)) << "时间戳格式不符：" << samples[i];
    if (i == 0) {
      continue;
    }
    EXPECT_LE(samples[i - 1], samples[i]) << "第 " << i << " 个时间戳相对前一个发生回退：" << samples[i - 1]
                                          << " -> " << samples[i];
  }
}

/// @brief 毫秒位须真实反映经过的时间，而非固定为 0 或整秒
TEST(UtilsTest, GetTimestampHasMillisecondResolution) {
  const auto begin = std::chrono::steady_clock::now();
  const std::string first = Utils::GetTimestamp();

  while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(120)) {
  }

  const std::string second = Utils::GetTimestamp();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

  ParsedTimestamp parsed_first;
  ParsedTimestamp parsed_second;
  ASSERT_TRUE(ParseTimestamp(first, parsed_first)) << "时间戳格式不符：" << first;
  ASSERT_TRUE(ParseTimestamp(second, parsed_second)) << "时间戳格式不符：" << second;

  const auto span = ToMillis(parsed_second) - ToMillis(parsed_first);
  EXPECT_GE(span, 100) << "两次取值间隔不足，毫秒位可能未生效：" << first << " -> " << second;
  EXPECT_LE(span, elapsed + 100) << "两次取值间隔超出实际耗时：" << first << " -> " << second;
}

/// @brief 多线程并发取值不应产出格式错误的时间戳
TEST(UtilsTest, GetTimestampIsThreadSafe) {
  constexpr int kThreads = 8;
  constexpr int kCallsPerThread = 200;

  std::atomic<int> malformed{};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i{}; i < kThreads; ++i) {
    threads.emplace_back([&malformed, kCallsPerThread]() {
      for (int j{}; j < kCallsPerThread; ++j) {
        ParsedTimestamp parsed;
        if (ParseTimestamp(Utils::GetTimestamp(), parsed) == false) {
          malformed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(malformed.load(), 0);
}

}  // namespace jojo::record2::test
