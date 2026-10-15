#ifndef JOJO_RECORD2_RECORDER_HPP_
#define JOJO_RECORD2_RECORDER_HPP_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "fixed_buffer.hpp"

namespace jojo::record2 {

/// @brief BCD 启用状态
enum class BcdEnable {
  kEnable,   // 启用 BCD 记录
  kDisable,  // 禁用 BCD 记录
};

/// @brief 写线程状态
enum class WriteState {
  kOk,        // 文件可写，正常落盘
  kRetrying,  // 文件暂不可用，正在退避重试
  kFatal,     // 无法继续落盘，已停止接收数据
  kStopped,   // 写线程已正常退出（关机），不再接收数据
};

constexpr std::size_t kBufferSize = jojo::container::kMediumBufferSize;
using BufferType = jojo::container::FixedBuffer<kBufferSize>;

/// @brief 数据记录器
class Recorder {
 public:
  /// @brief 构造函数
  /// @param basic_file_name 基础文件名
  /// @param ext 扩展名（须非空，过渡文件名不含扩展名，据此区分两类文件）
  Recorder(const std::string& basic_file_name, const std::string& ext, BcdEnable bcd_enable);

  /// @brief 析构函数
  ~Recorder();

  /// @brief 保存数据，由外部调用
  /// @param data 地址（不应为nullptr）
  /// @param length 长度（＞0）
  /// @return 是否已接收；false 表示未接收（析构中/已停止接收/超长）或 `length==0` 或 `data==nullptr`
  ///         或入队时丢弃了某个已满缓存（丢弃量由 `GetDroppedBytes()`获取）
  bool Save(const std::uint8_t* data, const std::size_t length);

  /// @brief 写线程状态，kStopped/kFatal 均表示已停止接收数据
  WriteState GetWriteState() const;

  /// @brief 未落盘的总字节数（best-effort，若ofs.write失败则无法正确计算）
  std::size_t GetDroppedBytes() const { return dropped_bytes_.load(std::memory_order_acquire); }

 private:
  /// @brief 数据落盘
  void Write();

  /// @brief 空缓存队列入队（单缓冲）
  void PushToFreeQueue(std::unique_ptr<BufferType>&& elem);

  /// @brief 空缓存队列入队（缓冲队列）
  void PushToFreeQueue(std::deque<std::unique_ptr<BufferType>>& queue);

  /// @brief 空缓存队列出队
  std::unique_ptr<BufferType> PopFromFreeQueue();

  /// @brief 满缓存队列入队，视条件通知落盘
  bool PushToFullQueue(std::unique_ptr<BufferType>&& buffer);

  /// @brief 满缓存队列入队，不受有界队列限制（供析构时交付最后一段数据）
  void PushToFullQueueForced(std::unique_ptr<BufferType>&& buffer);

  /// @brief 申请缓冲
  std::unique_ptr<BufferType> AllocateBuffer();

  /// @brief 累计一次文件打开失败，返回是否已达重试上限
  bool ExceedFileOpenRetryLimit();

  /// @brief 关闭文件并按"建立时间戳_关闭时间戳"定型文件名
  /// @param ofs 文件流
  /// @param filepath 待定型的文件路径
  void FinalizeFile(std::fstream& ofs, const std::filesystem::path& filepath);

  /// @brief 文件轮转
  /// @param ofs 文件流
  /// @param old_filepath 旧文件路径（用于重命名）
  void RotateFile(std::fstream& ofs, const std::filesystem::path& old_filepath);

  /// @brief 开始新文件段：记录文件建立时间戳，并分配未被占用的过渡文件名
  void StartSegment();

  /// @brief 追加中的过渡文件名（不含扩展名）：基础文件名_文件建立时间戳
  std::string FilenameStage1() const;

  /// @brief 文件关闭后的定型文件名：基础文件名_建立时间戳_关闭时间戳 + 扩展名
  std::string FilenameStage2() const;

 private:
  std::string basic_filename_;    // 基础文件名，在此基础上增加序号、时间戳等扩展
  std::string ext_;               // 扩展名，须非空以区分过渡文件名与定型文件名
  std::string current_filename_;  // 当前文件名

  std::string create_time_;  // 当前文件段的建立时间戳

  std::thread thread_;  // 写文件线程
  std::atomic<bool> accepting_{true};

  std::atomic<WriteState> write_state_{WriteState::kOk};  // 写线程状态，供 Save 与外部查询

  mutable std::mutex lock_current_buffer_;        // 保护 current_buffer_
  std::unique_ptr<BufferType> current_buffer_{};  // 当前可用缓存

  mutable std::mutex lock_free_queue_;                    // 保护 free_queue_
  std::deque<std::unique_ptr<BufferType>> free_queue_{};  // 空缓存队列

  mutable std::mutex lock_full_queue_{};                  // 保护 full_queue_
  std::deque<std::unique_ptr<BufferType>> full_queue_{};  // 满缓存队列
  std::condition_variable cv_write_file_{};               // 条件变量，通知写文件

  BcdEnable bcd_enable_;  // BCD 启用状态

  std::atomic<int> file_open_retry_times_{};  // 文件打开失败累计次数
  static inline constexpr std::size_t kMaxFullQueueSize{50};

  std::atomic<std::size_t>
      dropped_bytes_{};  // 任何原因导致的不会落盘的字节数（best-effort，若ofs.write失败则无法正确计算）
};

}  // namespace jojo::record2
#endif  // !JOJO_RECORD2_RECORDER_HPP_