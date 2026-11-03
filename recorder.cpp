#include "recorder.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>
#include <thread>

namespace {
constexpr static inline std::size_t kQueueSizeNotifyLimit{5};  // 满缓存队列容量通知上限
constexpr static inline std::size_t kMaxFullQueueSize{50};     // 满缓存容量上限
constexpr static inline std::chrono::seconds kTimeLimit{5};    // 落盘周期
constexpr static inline int kFileOpenRetryTimesLimit{10};      // 尝试打开文件次数上限
constexpr static inline int kWriteFailTimesLimit{10};          // 连续写失败次数上限
constexpr static inline std::size_t kMaxFileSize{2147483648};  // 单文件上限（2GB），超过就换下一个文件

/// @brief 路径是否存在；查询出错时按不存在处理
bool PathExists(const std::string& path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  return ec.value() == 0 && exists;
}

/// @brief 获取当前时间戳字符串，本地时间，毫秒精度
/// @return 形如 20261015T104827.321 的时间戳字符串
std::string GetTimestamp() {
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
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d.%03d", local_time.tm_year + 1900,
                local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                static_cast<int>(millis));
  return buffer;
}
}  // namespace

namespace jojo::record2 {

/// @brief 构造函数
Recorder::Recorder(const std::string& basic_filename, const std::string& ext)
    : basic_filename_{basic_filename}, ext_{ext} {
  current_buffer_ = AcquireBufferNoLock();
  InitFilename();
  thread_ = std::thread([this]() { this->WriteLoop(); });
}

/// @brief 析构函数
Recorder::~Recorder() {
  // 若 Recorder 使用智能指针管理，那么当进入析构函数，则智能指针的引用计数一定降为了 0，不会发生析构中调用 Save
  // 函数的情况

  // 将当前缓存写入 full_queue_，写入文件
  std::unique_ptr<BufferType> pending{};
  {
    std::scoped_lock lock{lock_buffers_};
    pending = std::move(current_buffer_);
  }

  {
    std::scoped_lock lock{lock_full_queue_};
    if (pending != nullptr) {
      full_queue_.push_back(std::move(pending));  // 最后一段不受队列上限限制，因此不使用 PushToFullQueue
    }
    accepting_.store(false, std::memory_order_release);
  }

  // 通知写入文件，线程自然退出
  cv_write_file_.notify_one();
  thread_.join();

  DeallocBuffers();
}

/// @brief 保存数据，由外部调用
bool Recorder::Save(const std::uint8_t* data, const std::size_t length) {
  if (data == nullptr || length == 0) {
    return false;
  }

  if (length > kBufferSize) {  // 超过缓存容量
    return false;
  }

  // 已停机就不再接收
  if (IsRejectingInput() == true) {
    return false;
  }

  // 避免同时持有两把锁，引入 pending_to_full_queue
  std::unique_ptr<BufferType> pending_to_full_queue{};
  {
    std::scoped_lock lock{lock_buffers_};
    if (current_buffer_ == nullptr) {
      current_buffer_ = AcquireBufferNoLock();
    }
    if (current_buffer_->Avail() < length) {  // 当前缓存放不下，换一块
      pending_to_full_queue = std::move(current_buffer_);
      current_buffer_ = AcquireBufferNoLock();
    }
    current_buffer_->Append(data, length);
  }

  if (pending_to_full_queue != nullptr) {
    return PushToFullQueue(std::move(pending_to_full_queue));
  }
  return true;
}

/// @brief 是否已停止接收数据（正常终止、致命错误，或析构已开始）
bool Recorder::IsRejectingInput() const {
  const WriteIoState state = write_io_state_.load(std::memory_order_acquire);
  return accepting_.load(std::memory_order_acquire) == false || state == WriteIoState::kFatal ||
         state == WriteIoState::kStopped;
}

/// @brief 写线程主体，停机时返回
void Recorder::WriteLoop() {
  std::deque<std::unique_ptr<BufferType>> local_free_queue;
  std::deque<std::unique_ptr<BufferType>> local_full_queue;
  std::fstream ofs;

  try {
    OpenFile(ofs);  // 打开失败则在下面的重试分支里处理
    while (true) {
      if (ofs.is_open() == false) {  // 文件不可用，退避重试
        if (accepting_.load(std::memory_order_acquire) == false) {
          break;
        }
        if (ExceedFileOpenRetryLimit() == true) {
          last_error_.store("发生致命错误，文件打开失败后重试次数达上限", std::memory_order_release);
          write_io_state_.store(WriteIoState::kFatal, std::memory_order_release);
          break;
        } else {
          write_io_state_.store(WriteIoState::kRetrying, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 等 100ms 再试！
          OpenFile(ofs);
          continue;
        }
      }

      // 文件打开成功
      file_open_retry_times_ = 0;
      if (write_fail_times_ == 0) {
        write_io_state_.store(WriteIoState::kOk, std::memory_order_release);
      }

      {
        std::unique_lock lk{lock_full_queue_};
        // 若上一轮 local_full_queue 的数据没有写完，则立即继续写，延后对 cv_write_file_的监听
        if (local_full_queue.empty() == true) {
          const bool cv_signaled = cv_write_file_.wait_for(lk, std::chrono::seconds(kTimeLimit), [this]() {
            return (full_queue_.empty() == false) || (accepting_.load(std::memory_order_acquire) == false);
          });

          // 超时，当前缓存加入 full_queue_
          if (cv_signaled == false) {
            lk.unlock();  // PushToFullQueue 中使用了 lock_full_queue_，此处暂时解锁+避免2把锁同时加锁
            std::unique_ptr<BufferType> pending;
            {
              std::scoped_lock lock{lock_buffers_};
              pending = std::move(current_buffer_);
            }
            PushToFullQueue(std::move(pending));
            lk.lock();  // 取队列和判退要在同一临界区，否则会漏掉析构推入的最后一段
          }
        }

        if (local_full_queue.empty() == true) {
          local_full_queue.swap(full_queue_);  // 仍在 lock_full_queue_ 的保护下，直接交换
        } else {                               // 上批没写完，full_queue_ 元素挂到队尾
          while (full_queue_.empty() == false) {
            local_full_queue.push_back(std::move(full_queue_.front()));
            full_queue_.pop_front();
          }
        }

        // 已停止接收且无待写数据，退出阻塞循环（while(true))
        if (local_full_queue.empty() == true && accepting_.load(std::memory_order_acquire) == false) {
          break;  // while (true)
        }
      }  // 受 lock_full_queue_ 保护的临界区结束

      if (local_full_queue.empty() == true) {
        continue;
      }

      // lock_full_queue_ 锁外写盘
      bool give_up_write_file = false;  // 写失败次数达上限，放弃落盘
      while (local_full_queue.empty() == false) {
        // 单缓冲出队
        auto buffer = std::move(local_full_queue.front());
        local_full_queue.pop_front();
        if (buffer == nullptr || buffer->Length() == 0) {
          continue;
        }

        ofs.write(reinterpret_cast<const char*>(buffer->Data()), buffer->Length());
        if (ofs.good() == false) {  // 写失败，失败的这块丢弃，其余留在 local_full_queue 里下轮重试
          break;                    // 流的失败状态是粘性的，下面直接按 ofs.good() 判断，不用另设标志
        }
        filesize_ += buffer->Length();

        // 按需轮转文件
        if (filesize_ >= kMaxFileSize) {
          RotateFile(ofs, current_filename_);  // 其中调用了 OpenFile，注意判断文件打开结果

          if (ofs.is_open() == false) {  // 重开失败，回退重试
            write_io_state_.store(WriteIoState::kRetrying, std::memory_order_release);
            break;
          }
        }
        local_free_queue.push_back(std::move(buffer));
      }  // while (local_full_queue.empty() == false)

      if (ofs.is_open() == true) {  // is_open 为假说明轮转后重开失败，交给上面的重试分支
        if (ofs.good() == true && local_free_queue.empty() == false) {
          ofs.flush();  // 把 C++ 流对象内部的缓冲区中的数据，通过底层系统调用交给操作系统，C++工作完成
        }
        if (ofs.good() == false) {  // 写或刷失败：关文件，按累计次数决定重试还是放弃
          ofs.close();              // close 会尽量刷出已缓冲数据
          if (ExceedWriteFailLimit() == true) {
            last_error_.store("发生致命错误，写文件连续失败次数达上限", std::memory_order_release);
            write_io_state_.store(WriteIoState::kFatal, std::memory_order_release);
            give_up_write_file = true;
          } else {
            write_io_state_.store(WriteIoState::kRetrying, std::memory_order_release);
          }
        } else if (local_free_queue.empty() == false) {
          write_fail_times_ = 0;  // 这一批确实写成功了
          write_io_state_.store(WriteIoState::kOk, std::memory_order_release);
        }
      }

      PushToFreeQueue(local_free_queue);  // 空队列直接返回

      if (give_up_write_file == true) {
        break;  // while (true)
      }
    }  // while (true) 边界
  } catch (const std::bad_alloc&) {
    last_error_.store("发生致命错误，缓存分配失败", std::memory_order_release);
    write_io_state_.store(WriteIoState::kFatal, std::memory_order_release);
  } catch (...) {
    last_error_.store("发生致命错误，发生未预期异常", std::memory_order_release);
    write_io_state_.store(WriteIoState::kFatal, std::memory_order_release);
  }

  // 退出循环则 write_io_state_只有两种可能：
  // kFatal：致命错误
  // kStopped：其他
  if (write_io_state_.load(std::memory_order_acquire) != WriteIoState::kFatal) {
    write_io_state_.store(WriteIoState::kStopped, std::memory_order_release);
  }

  // 关闭文件并更改文件名
  try {
    FinalizeFile(ofs, current_filename_);
  } catch (...) {
  }

  // 释放内存
  local_free_queue.clear();
  local_full_queue.clear();
  DeallocBuffers();
}

/// @brief 释放所有缓存
void Recorder::DeallocBuffers() {
  {
    std::scoped_lock lock{lock_buffers_};
    current_buffer_ = nullptr;
    free_queue_.clear();  // 清除元素，但队列内存不会释放
  }
  {
    std::scoped_lock lock{lock_full_queue_};
    full_queue_.clear();
  }
}

void Recorder::PushToFreeQueue(std::unique_ptr<BufferType>&& elem) {
  if (elem == nullptr) {
    return;
  }
  elem->Reset();  // FixedBuffer复位，不是 unique_ptr
  {
    std::scoped_lock lock{lock_buffers_};
    free_queue_.push_back(std::move(elem));
  }
}

void Recorder::PushToFreeQueue(std::deque<std::unique_ptr<BufferType>>& queue) {
  if (queue.empty() == true) {
    return;
  }

  std::scoped_lock lock{lock_buffers_};
  while (queue.empty() == false) {
    auto buffer = std::move(queue.front());
    queue.pop_front();
    buffer->Reset();
    free_queue_.push_back(std::move(buffer));
  }
}

std::unique_ptr<BufferType> Recorder::AcquireBufferNoLock() {
  if (free_queue_.empty() == false) {
    auto elem = std::move(free_queue_.front());
    free_queue_.pop_front();
    return elem;
  }
  return AllocateBuffer();
}

/// @brief 入满队列；队列已满则丢弃该缓存并返回 false
bool Recorder::PushToFullQueue(std::unique_ptr<BufferType>&& buffer) {
  if (buffer == nullptr) {
    return false;
  }

  bool pushed = false;
  bool notify = false;
  {
    std::scoped_lock lock{lock_full_queue_};
    if (full_queue_.size() < kMaxFullQueueSize) {
      full_queue_.push_back(std::move(buffer));
      pushed = true;
      notify = full_queue_.size() >= kQueueSizeNotifyLimit;
    }
  }

  if (pushed == false) {                 // 满缓存队列满了，丢弃这块的数据
    PushToFreeQueue(std::move(buffer));  // 缓存回收（复位由入队函数负责）
    return false;
  }
  if (notify == true) {
    cv_write_file_.notify_one();  // 放到锁外，免得被唤醒的写线程立刻又阻塞在这把锁上
  }
  return true;
}

/// @brief 申请缓冲
std::unique_ptr<BufferType> Recorder::AllocateBuffer() { return std::unique_ptr<BufferType>(new BufferType); }

/// @brief 写线程状态
WriteIoState Recorder::GetWriteState() const { return write_io_state_.load(std::memory_order_acquire); }

/// @brief 当前段文件大小，取不到时为 0
std::size_t Recorder::QueryFileSize() const {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(current_filename_, ec);
  return ec ? 0 : static_cast<std::size_t>(size);
}

/// @brief 以追加方式打开当前文件；成功时同步 filesize_。调用方负责判断文件打开结果
bool Recorder::OpenFile(std::fstream& ofs) {
  ofs.open(current_filename_, std::ios::binary | std::ios::app);  // open 成功时内部会 clear 状态位
  if (ofs.is_open() == false) {
    return false;
  }
  filesize_ = QueryFileSize();  // 上次可能只写进去一部分，重新对齐大小
  return true;
}

/// @brief 轮转文件
void Recorder::RotateFile(std::fstream& ofs, const std::filesystem::path& old_filepath) {
  FinalizeFile(ofs, old_filepath);
  InitFilename();
  OpenFile(ofs);  // 失败由调用方按 is_open() 判断
}

/// @brief flush/close，并按"建立_关闭"时间戳改名
void Recorder::FinalizeFile(std::fstream& ofs, const std::filesystem::path& filepath) {
  ofs.close();  // close 内部会先 flush

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(filepath, ec);  // 不存在或不可访问时 ec 非零
  if (ec) {
    return;
  }
  if (size == 0) {  // 不保留空文件
    std::filesystem::remove(filepath, ec);
  } else {
    std::filesystem::rename(filepath, FinalFilename(), ec);  // 改名失败就留着过渡名，数据还在
  }
}

/// @brief 初始文件名（基础名_创建时间_序号（按需））
void Recorder::InitFilename() {
  create_time_ = GetTimestamp();
  std::string name{basic_filename_ + "_" + create_time_};

  // 有同名文件时加序号，仅做临时区分，FinalFilename中由关闭时间替代
  for (std::size_t i{1}; std::filesystem::exists(name); ++i) {
    name = basic_filename_ + "_" + create_time_ + "_" + std::to_string(i);
  }

  current_filename_ = name;
}

/// @brief 最终文件名（基础名_创建时间_关闭时间 + 扩展名；重名时追加序号，避免覆盖）
std::string Recorder::FinalFilename() const {
  const std::string stem = basic_filename_ + "_" + create_time_ + "_" + GetTimestamp();
  std::string name{stem + ext_};
  for (std::size_t i{1}; PathExists(name) == true; ++i) {
    name = stem + "_" + std::to_string(i) + ext_;
  }
  return name;
}

/// @brief 累计一次文件打开失败，返回是否已达重试上限
bool Recorder::ExceedFileOpenRetryLimit() {
  ++file_open_retry_times_;
  return file_open_retry_times_ >= kFileOpenRetryTimesLimit;
}

/// @brief 累计一次写失败，返回是否已达重试上限
bool Recorder::ExceedWriteFailLimit() {
  ++write_fail_times_;
  return write_fail_times_ >= kWriteFailTimesLimit;
}

}  // namespace jojo::record2
