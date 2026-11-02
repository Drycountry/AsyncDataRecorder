#include "test_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

namespace jojo::record2::test {
namespace {

/// @brief 生成不重复的临时目录名，避免多个测试进程或多次运行相互干扰
std::string UniqueDirName() {
  static std::atomic<unsigned> counter{};
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return "jojo_record2_test_" + std::to_string(stamp) + "_" + std::to_string(counter++);
}

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// @brief 按基础文件名与扩展名筛选目录下的文件
/// @param finalized true 取定型文件（带扩展名），false 取过渡文件（不带扩展名）
std::vector<std::filesystem::path> FindFiles(const std::filesystem::path& dir,
                                             const std::string& basic_filename, const std::string& ext,
                                             bool finalized) {
  std::vector<std::filesystem::path> out;

  const std::string prefix = basic_filename + "_";
  std::error_code ec{};
  for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
    if (entry.is_regular_file(ec) == false) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (StartsWith(name, prefix) == false) {
      continue;
    }
    if (EndsWith(name, ext) == finalized) {
      out.push_back(entry.path());
    }
  }

  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

void TempWorkDir::SetUp() {
  origin_dir_ = std::filesystem::current_path();
  dir_ = std::filesystem::temp_directory_path() / UniqueDirName();
  std::filesystem::create_directories(dir_);
  std::filesystem::current_path(dir_);
}

void TempWorkDir::TearDown() {
  // 先切回原工作目录，再清理本用例产生的数据文件
  std::filesystem::current_path(origin_dir_);

  std::error_code ec{};
  std::filesystem::remove_all(dir_, ec);
  EXPECT_EQ(ec.value(), 0) << "清理临时目录失败：" << dir_.string() << "，" << ec.message();
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream ifs{path, std::ios::binary};
  if (ifs.is_open() == false) {
    return {};
  }
  return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{ifs}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> MakeBlock(std::size_t count, std::uint8_t value) {
  return std::vector<std::uint8_t>(count, value);
}

::testing::AssertionResult BytesEqual(const std::vector<std::uint8_t>& actual,
                                      const std::vector<std::uint8_t>& expected) {
  if (actual.size() != expected.size()) {
    return ::testing::AssertionFailure() << "长度不一致，实际 " << actual.size() << " 字节，期望 "
                                         << expected.size() << " 字节";
  }

  const auto mismatch = std::mismatch(actual.cbegin(), actual.cend(), expected.cbegin());
  if (mismatch.first == actual.cend()) {
    return ::testing::AssertionSuccess();
  }

  const auto offset = static_cast<std::size_t>(std::distance(actual.cbegin(), mismatch.first));
  return ::testing::AssertionFailure()
         << "第 " << offset << " 字节起不一致，实际 " << static_cast<int>(*mismatch.first) << "，期望 "
         << static_cast<int>(*mismatch.second);
}

/// @brief 列举目录下的定型文件（基础文件名_建立时间戳_关闭时间戳 + 扩展名）
std::vector<std::filesystem::path> FindFinalizedFiles(const std::filesystem::path& dir,
                                                      const std::string& basic_filename,
                                                      const std::string& ext) {
  return FindFiles(dir, basic_filename, ext, true);
}

/// @brief 列举目录下的过渡文件（正在追加，尚无扩展名）
std::vector<std::filesystem::path> FindTransitionalFiles(const std::filesystem::path& dir,
                                                         const std::string& basic_filename,
                                                         const std::string& ext) {
  return FindFiles(dir, basic_filename, ext, false);
}

/// @brief 轮询等待过渡文件出现（写线程异步建立文件），超时返回空路径
std::filesystem::path WaitForTransitionalFile(const std::filesystem::path& dir,
                                              const std::string& basic_filename, const std::string& ext,
                                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto found = FindTransitionalFiles(dir, basic_filename, ext);
    if (found.empty() == false) {
      return found.front();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/// @brief 轮询等待文件长度不小于下限
bool WaitForFileSizeAtLeast(const std::filesystem::path& path, std::uintmax_t min_size,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    std::error_code ec{};
    const auto size = std::filesystem::file_size(path, ec);
    if (ec.value() == 0 && size >= min_size) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace jojo::record2::test
