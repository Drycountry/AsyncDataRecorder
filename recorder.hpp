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
#include <shared_mutex>
#include <string>
#include <thread>

#include "fixed_buffer.hpp"

namespace jojo::record2 {

/// @brief 写线程状态
enum class WriteIoState {
  kOk,        // 正常接收数据与写文件
  kRetrying,  // 文件打开或写文件失败，正在重试
  kFatal,     // 致命错误，不再接收数据
  kStopped,   // 正常终止，不再接收数据
};

constexpr std::size_t kBufferSize = jojo::container::kMediumBufferSize;
using BufferType = jojo::container::FixedBuffer<kBufferSize>;

/// @brief 数据记录器
class Recorder {
 public:
  /// @brief 构造函数
  /// @param basic_file_name 基础文件名
  /// @param ext 扩展名（须非空，过渡文件名不含扩展名，据此区分两类文件）
  Recorder(const std::string& basic_file_name, const std::string& ext);

  /// @brief 析构函数
  ~Recorder();

  /// @brief 停止接收数据，完成落盘并等待写线程结束；可重复调用
  void Close() noexcept;

  /// @brief 保存数据，由外部调用
  /// @return false：未接收（参数非法、已停机、超过缓存容量），或队列满丢了更早的缓存
  bool Save(const std::uint8_t* data, const std::size_t length);

  /// @brief 写线程状态
  WriteIoState GetWriteState() const;

  /// @brief 获取致命错误信息
  const char* GetLastError() const { return last_error_.load(std::memory_order_acquire); }

 private:
  /// @brief 是否已停止接收数据
  bool IsRejectingInput() const;

  /// @brief 写线程主体，停机时返回
  void WriteLoop();

  /// @note 调用方须持有 lock_buffers_
  std::unique_ptr<BufferType> AcquireBufferNoLock();

  /// @brief 空缓存队列入队
  void PushToFreeQueue(std::unique_ptr<BufferType>&& elem);

  /// @brief 空缓存队列入队
  void PushToFreeQueue(std::deque<std::unique_ptr<BufferType>>& queue);

  /// @brief 入满队列，视长度通知写线程
  bool PushToFullQueue(std::unique_ptr<BufferType>&& buffer);

  /// @brief 释放缓存池和满队列
  void DeallocBuffers();

  /// @brief 申请缓冲
  std::unique_ptr<BufferType> AllocateBuffer();

  /// @brief 累计一次打开失败，返回是否已达上限
  bool ExceedFileOpenRetryLimit();

  /// @brief 累计一次写失败，返回是否已达上限
  bool ExceedWriteFailLimit();

  /// @brief 以追加方式打开当前段文件；成功时同步 filesize_
  bool OpenFile(std::fstream& ofs);

  /// @brief 当前段文件大小，取不到时为 0
  std::size_t QueryFileSize() const;

  /// @brief flush/close，并按"建立_关闭"时间戳改名
  void FinalizeFile(std::fstream& ofs, const std::filesystem::path& filepath);

  /// @brief 关当前段，开下一段
  void RotateFile(std::fstream& ofs, const std::filesystem::path& old_filepath);

  /// @brief 取建立时间戳，挑一个没被占用的过渡文件名
  void InitFilename();

  /// @brief 文件关闭后的定型文件名：基础文件名_建立时间戳_关闭时间戳 + 扩展名；重名时追加序号
  std::string FinalFilename() const;

 private:
  std::string basic_filename_;    // 基础文件名，在此基础上增加序号、时间戳等扩展
  std::string ext_;               // 扩展名，须非空以区分过渡文件名与定型文件名
  std::string current_filename_;  // 当前文件名

  std::string create_time_;  // 本段建立时间戳
  std::size_t filesize_{};   // 已交给流的字节数（估计值），仅写线程访问；只用作轮转判断，重开文件时按实际大小重新对齐

  std::thread thread_;                 // 写文件线程
  std::atomic<bool> accepting_{true};  // 是否接收数据

  std::atomic<WriteIoState> write_io_state_{WriteIoState::kOk};  // 供 Save 与外部查询
  std::atomic<const char*> last_error_{nullptr};                 // 致命错误信息

  mutable std::shared_mutex lifecycle_lock_;              // Save 共享、Close 独占，保证关闭安全
  mutable std::mutex lock_buffers_;                       // 保护 current_buffer_ 与 free_queue_
  std::unique_ptr<BufferType> current_buffer_{};          // 当前可用缓存
  std::deque<std::unique_ptr<BufferType>> free_queue_{};  // 空缓存队列

  mutable std::mutex lock_full_queue_{};                  // 保护 full_queue_
  std::deque<std::unique_ptr<BufferType>> full_queue_{};  // 满缓存队列
  std::condition_variable cv_write_file_{};               // 条件变量，通知写文件

  int file_open_retry_times_{};  // 文件打开失败累计次数，仅写线程访问
  int write_fail_times_{};       // 连续写失败次数，写成功后清零，仅写线程访问
};

}  // namespace jojo::record2
#endif  // !JOJO_RECORD2_RECORDER_HPP_
