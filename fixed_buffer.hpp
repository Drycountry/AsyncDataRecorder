#ifndef JOJO_FOUNDATION_CONTAINER_FIXED_BUFFER_H_
#define JOJO_FOUNDATION_CONTAINER_FIXED_BUFFER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jojo::container {

constexpr int kSmallBufferSize = 1024 * 10;
constexpr int kMediumBufferSize = 1024 * 100;
constexpr int kLargeBufferSize = 1024 * 1000;

template <int SIZE>
class FixedBuffer {
 public:
  FixedBuffer() = default;
  ~FixedBuffer() = default;
  FixedBuffer(const FixedBuffer &) = delete;
  FixedBuffer(FixedBuffer &&) = delete;
  FixedBuffer operator=(const FixedBuffer &) = delete;
  FixedBuffer operator=(FixedBuffer &&) = delete;
  /// @brief 追加数据到内部数组尾部
  void Append(const std::uint8_t *buf, size_t len) {
    if (Avail() >= len) {
      std::memcpy(data_.data() + cur_, buf, len);
      cur_ += static_cast<int>(len);  // len ≤ Avail() ≤ SIZE，转换不会溢出
    }
  }

  /// @brief 获取内部数组的指针
  const std::uint8_t *Data() const { return data_.data(); }
  /// @brief 已写入数据长度
  int Length() const { return cur_; }
  /// @brief 可用空间大小
  int Avail() const { return SIZE - cur_; }
  /// @brief 复位
  void Reset() { cur_ = 0; }
  /// @brief 内部数组内容置为0
  void Clear() { std::memset(data_.data(), 0, SIZE); }

 private:
  std::array<std::uint8_t, SIZE> data_;
  int cur_{};
};

}  // namespace jojo::container

#endif  // !JOJO_FOUNDATION_CONTAINER_FIXED_BUFFER_H_
