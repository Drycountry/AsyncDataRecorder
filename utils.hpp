#ifndef JOJO_RECORD2_UTILS_HPP_
#define JOJO_RECORD2_UTILS_HPP_

#include <string>

namespace jojo::record2 {

/// @brief 通用工具
class Utils {
 public:
  /// @brief 获取当前时间戳字符串，本地时间，毫秒精度
  /// @return 形如 20261015T104827.321 的时间戳字符串
  static std::string GetTimestamp();
};

}  // namespace jojo::record2
#endif  // !JOJO_RECORD2_UTILS_HPP_
