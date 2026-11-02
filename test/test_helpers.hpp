#ifndef JOJO_RECORD2_TEST_HELPERS_HPP_
#define JOJO_RECORD2_TEST_HELPERS_HPP_

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jojo::record2::test {

/// @brief 测试用工作目录夹具
///        Recorder 按相对路径落盘，故把进程工作目录切到以用例为单位的临时目录，
///        既隔离各用例产生的数据文件，也便于用例结束后一并清理
class TempWorkDir : public ::testing::Test {
 protected:
  void SetUp() override;
  void TearDown() override;

  /// @brief 当前用例独享的工作目录
  const std::filesystem::path& dir() const { return dir_; }

 private:
  std::filesystem::path origin_dir_{};  // 切换前的工作目录
  std::filesystem::path dir_{};         // 本用例独享的临时工作目录
};

/// @brief 读取文件全部内容，文件不可读时返回空
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);

/// @brief 生成 count 个值均为 value 的数据块
std::vector<std::uint8_t> MakeBlock(std::size_t count, std::uint8_t value);

/// @brief 比较字节内容，失败时定位到首个不一致位置，避免大块数据打印刷屏
::testing::AssertionResult BytesEqual(const std::vector<std::uint8_t>& actual,
                                      const std::vector<std::uint8_t>& expected);

/// @brief 列举目录下的定型文件（基础文件名_建立时间戳_关闭时间戳 + 扩展名）
/// @param ext 扩展名，须非空（过渡文件不含扩展名，据此区分两类文件）
std::vector<std::filesystem::path> FindFinalizedFiles(const std::filesystem::path& dir,
                                                      const std::string& basic_filename,
                                                      const std::string& ext);

/// @brief 列举目录下的过渡文件（正在追加，尚无扩展名）
/// @param ext 扩展名，须非空（过渡文件不含扩展名，据此区分两类文件）
std::vector<std::filesystem::path> FindTransitionalFiles(const std::filesystem::path& dir,
                                                         const std::string& basic_filename,
                                                         const std::string& ext);

/// @brief 轮询等待过渡文件出现（写线程异步建立文件），超时返回空路径
std::filesystem::path WaitForTransitionalFile(const std::filesystem::path& dir,
                                              const std::string& basic_filename, const std::string& ext,
                                              std::chrono::milliseconds timeout);

/// @brief 轮询等待文件长度不小于下限
bool WaitForFileSizeAtLeast(const std::filesystem::path& path, std::uintmax_t min_size,
                            std::chrono::milliseconds timeout);

}  // namespace jojo::record2::test
#endif  // !JOJO_RECORD2_TEST_HELPERS_HPP_
