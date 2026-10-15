#include "utils.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace jojo::record2 {

/// @brief 获取当前时间戳字符串，本地时间，毫秒精度
/// @return 形如 20261015T104827.321 的时间戳字符串
std::string Utils::GetTimestamp() {
  const auto now = std::chrono::system_clock::now();

  // 取整秒与不足 1s 的毫秒部分，避免负时间点（1970 之前）截断取整出错
  const auto seconds_point = std::chrono::floor<std::chrono::seconds>(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds_point).count();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(seconds_point);

  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &seconds);  // Windows 下的线程安全版本
#else
  localtime_r(&seconds, &local_time);  // Linux 下的线程安全版本
#endif

  char buffer[24]{};  // yyyyMMddTHHmmss.fff 共 19 字符，留余量
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d.%03d",
                local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
                local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                static_cast<int>(millis));
  return buffer;
}

}  // namespace jojo::record2
