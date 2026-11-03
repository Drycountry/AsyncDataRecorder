#ifndef JOJO_RECORD2_RECORDER_MANAGER_HPP_
#define JOJO_RECORD2_RECORDER_MANAGER_HPP_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "recorder.hpp"

namespace jojo::record2 {

/// @brief 记录器创建参数
struct RecorderOptions {
  std::filesystem::path output_directory{"."};
  std::string extension{".dat"};
};

class RecorderManager {
 public:
  explicit RecorderManager(RecorderOptions options = {});
  ~RecorderManager();

  /// @brief 按 id 获取记录器，不存在时创建
  std::shared_ptr<Recorder> CreateRecorder(std::string_view id);

  /// @brief 查询已有记录器，不存在或 id 非法时返回空指针
  std::shared_ptr<Recorder> FindRecorder(std::string_view id) const;

  /// @brief 关闭并移除指定记录器；记录器不存在或 id 非法时返回 false
  bool CloseRecorder(std::string_view id);

  /// @brief 关闭并移除全部记录器，返回关闭数量
  std::size_t CloseAllRecorders();

  bool ContainsRecorder(std::string_view id) const;
  std::size_t Size() const;

 private:
  RecorderOptions options_;
  mutable std::mutex lock_recorders_;
  std::unordered_map<std::string, std::shared_ptr<Recorder>> recorders_{};
};
}  // namespace jojo::record2

#endif  // !JOJO_RECORD2_RECORDER_MANAGER_HPP_
