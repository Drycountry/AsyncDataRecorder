#include "recorder.hpp"

#include <chrono>
#include <filesystem>
#include <new>
#include <string>
#include <thread>

#include "utils.hpp"

/// @brief 匿名命名空间，仅在当前编译单元有效
namespace {
// 满队列攒到 5 个缓存就通知写线程落盘
constexpr static inline std::size_t kQueueSizeNotifyLimit{5};

// 每隔 5s 落盘 1 次
constexpr static inline std::chrono::seconds kTimeLimit{5};

// 打开失败重试上限，达到后放弃落盘（写线程进入 kFatal）
constexpr static inline int kFileOpenRetryTimesLimit{10};

// 单文件上限，超过就换下一个文件
constexpr static inline std::size_t kMaxFileSize{2147483648};  // 2GB
}  // namespace

namespace jojo::record2 {

/// @brief 构造函数
Recorder::Recorder(const std::string& basic_filename, const std::string& ext)
    : basic_filename_{basic_filename}, ext_{ext} {
  // 缓存数要比通知阈值多，入队时才有空闲缓存可用
  for (int i{}; i < kQueueSizeNotifyLimit + 1; ++i) {
    PushToFreeQueue(AllocateBuffer());
  }
  {  // 无竞争，加锁只为统一取缓存路径
    std::scoped_lock lock{lock_buffers_};
    current_buffer_ = AcquireBufferLocked();
  }
  AllocateSegmentName();

  thread_ = std::thread([this]() { this->WriteLoop(); });
}

/// @brief 析构函数
Recorder::~Recorder() {
  // 当前缓存交给写线程，保证不丢
  std::unique_ptr<BufferType> pending{};
  {
    std::scoped_lock lock{lock_buffers_};
    pending = std::move(current_buffer_);
  }
  PushToFullQueueForced(std::move(pending));

  // 必须在锁内改 accepting_，否则通知可能落在写线程"判完谓词、还没阻塞"之间丢掉，
  // 析构会白等一个 kTimeLimit
  {
    std::scoped_lock lock{lock_full_queue_};
    accepting_.store(false, std::memory_order_release);
  }
  cv_write_file_.notify_one();
  thread_.join();

  ReleasePendingBuffers();  // 写线程收尾后才入队的残留
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
  const WriteState state = write_state_.load(std::memory_order_acquire);
  if (accepting_.load(std::memory_order_acquire) == false || state == WriteState::kFatal ||
      state == WriteState::kStopped) {
    return false;
  }

  std::unique_ptr<BufferType> pending_to_full_queue{};  // 锁外再入队，避免两把锁嵌套
  {
    std::scoped_lock lock{lock_buffers_};
    // 写线程先置状态再抢这把锁，这里再判一次就不会漏
    const WriteState current = write_state_.load(std::memory_order_acquire);
    if (accepting_.load(std::memory_order_acquire) == false || current == WriteState::kFatal ||
        current == WriteState::kStopped) {
      return false;
    }
    if (current_buffer_ == nullptr) {
      current_buffer_ = AcquireBufferLocked();
    }
    if (current_buffer_->Avail() < length) {  // 当前缓存放不下，换一块
      pending_to_full_queue = std::move(current_buffer_);
      current_buffer_ = AcquireBufferLocked();
    }
    current_buffer_->Append(data, length);
  }

  if (pending_to_full_queue != nullptr) {
    return PushToFullQueue(std::move(pending_to_full_queue));
  }
  return true;
}

/// @brief 写线程主体，停机时返回
void Recorder::WriteLoop() {
  std::deque<std::unique_ptr<BufferType>> local_free_queue;
  std::deque<std::unique_ptr<BufferType>> batch;  // 锁内换出，锁外写盘

  std::fstream ofs(current_filename_, std::ios::binary | std::ios::app);
  filesize_ = ofs.is_open() ? QueryFileSize() : 0;  // 接着文件已有的大小累加

  try {
    while (true) {
      if (ofs.is_open() == false) {  // 文件不可用，退避重试
        if (accepting_.load(std::memory_order_acquire) == false) {
          break;  // 关机：文件都没建成，没什么可定型
        }
        if (ExceedFileOpenRetryLimit() == true) {
          last_error_.store("文件打开失败重试达上限，已放弃落盘", std::memory_order_release);
          write_state_.store(WriteState::kFatal, std::memory_order_release);
          break;
        }
        write_state_.store(WriteState::kRetrying, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ofs.clear();
        ofs.open(current_filename_, std::ios::binary | std::ios::app);
        if (ofs.is_open() == true) {
          filesize_ = QueryFileSize();  // 上次可能写进去一部分，重新取大小
        }
        continue;
      }
      file_open_retry_times_ = 0;
      write_state_.store(WriteState::kOk, std::memory_order_release);

      bool finished{};
      {
        std::unique_lock lk{lock_full_queue_};
        const bool cv_signaled = cv_write_file_.wait_for(lk, std::chrono::seconds(kTimeLimit), [this]() -> bool {
          return (full_queue_.empty() == false) || (accepting_.load(std::memory_order_acquire) == false);
        });
        lk.unlock();  // 下面要取 lock_buffers_，先让开

        if (cv_signaled == false) {  // 超时，把当前缓存也交出去
          std::unique_ptr<BufferType> pending;
          {
            std::scoped_lock lock{lock_buffers_};
            pending = std::move(current_buffer_);
          }
          PushToFullQueue(std::move(pending));
        }

        lk.lock();  // 取队列和判退要在同一临界区，否则会漏掉析构推入的最后一段
        if (batch.empty() == true) {
          batch.swap(full_queue_);  // O(1)
        } else {                    // 上批没写完，接着往后排
          while (full_queue_.empty() == false) {
            batch.push_back(std::move(full_queue_.front()));
            full_queue_.pop_front();
          }
        }
        if (batch.empty() == true && accepting_.load(std::memory_order_acquire) == false) {
          finished = true;
        }
      }

      if (batch.empty() == true) {
        if (finished == true) {
          break;
        }
        continue;
      }

      while (batch.empty() == false) {  // 锁外写盘
        auto buffer = std::move(batch.front());
        batch.pop_front();
        if (buffer == nullptr) {
          continue;
        }

        ofs.write(reinterpret_cast<const char*>(buffer->Data()), buffer->Length());
        if (ofs.good() == false) {  // 写失败
          write_state_.store(WriteState::kRetrying, std::memory_order_release);
          ofs.clear();
          ofs.flush();
          ofs.close();
          break;  // 剩下的留在 batch 里，下一轮再写
        }
        filesize_ += buffer->Length();

        if (filesize_ >= kMaxFileSize) {  // 到上限就换文件
          RotateFile(ofs, current_filename_);
          filesize_ = 0;

          if (ofs.is_open() == false) {  // 重开失败，回退重试
            write_state_.store(WriteState::kRetrying, std::memory_order_release);
            break;
          }
        }
        local_free_queue.push_back(std::move(buffer));
      }

      // 这一批写完了，flush 并把缓存收回空闲池
      if (local_free_queue.empty() == false) {
        if (ofs.is_open() == true) {
          ofs.flush();
          if (ofs.good() == false) {  // 这批没真正落盘
            write_state_.store(WriteState::kRetrying, std::memory_order_release);
            ofs.close();
            ofs.clear();
          }
        }
        PushToFreeQueue(local_free_queue);
      }
    }
  } catch (const std::bad_alloc&) {
    last_error_.store("写线程内存分配失败，已放弃落盘", std::memory_order_release);
    write_state_.store(WriteState::kFatal, std::memory_order_release);
  } catch (...) {
    last_error_.store("写线程发生未预期异常，已放弃落盘", std::memory_order_release);
    write_state_.store(WriteState::kFatal, std::memory_order_release);
  }

  if (write_state_.load(std::memory_order_acquire) != WriteState::kFatal) {  // kFatal 不覆盖
    write_state_.store(WriteState::kStopped, std::memory_order_release);
  }

  batch.clear();
  try {
    FinalizeFile(ofs, current_filename_);
  } catch (...) {  // 拼文件名要分配内存，失败就留着过渡名
  }

  ReleasePendingBuffers();
}

/// @brief 释放缓存池与满队列
void Recorder::ReleasePendingBuffers() {
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
  elem->Reset();
  {
    std::scoped_lock lock{lock_buffers_};
    free_queue_.push_back(std::move(elem));
  }
}

void Recorder::PushToFreeQueue(std::deque<std::unique_ptr<BufferType>>& queue) {
  if (queue.empty() == true) {
    return;  // 避免加锁
  }

  std::scoped_lock lock{lock_buffers_};
  while (queue.empty() == false) {
    auto buffer = std::move(queue.front());
    buffer->Reset();
    queue.pop_front();
    free_queue_.push_back(std::move(buffer));
  }
}

std::unique_ptr<BufferType> Recorder::AcquireBufferLocked() {
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

  {
    std::scoped_lock lock{lock_full_queue_};
    if (full_queue_.size() < kMaxFullQueueSize) {
      full_queue_.push_back(std::move(buffer));
      if (full_queue_.size() >= kQueueSizeNotifyLimit) {
        cv_write_file_.notify_one();
      }
      return true;
    }
  }

  buffer->Reset();  // 队列满了，丢掉这块，缓存回收
  PushToFreeQueue(std::move(buffer));
  return false;
}

/// @brief 入满队列，不受上限限制，供析构时交付最后一段
void Recorder::PushToFullQueueForced(std::unique_ptr<BufferType>&& buffer) {
  if (buffer == nullptr) {
    return;
  }

  std::scoped_lock lock{lock_full_queue_};
  full_queue_.push_back(std::move(buffer));
  cv_write_file_.notify_one();
}

std::unique_ptr<BufferType> Recorder::AllocateBuffer() { return std::make_unique<BufferType>(); }

/// @brief 写线程状态
WriteState Recorder::GetWriteState() const { return write_state_.load(std::memory_order_acquire); }

/// @brief 当前段文件大小，取不到时为 0
std::size_t Recorder::QueryFileSize() const {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(current_filename_, ec);
  return ec ? 0 : static_cast<std::size_t>(size);
}

/// @brief flush/close，并按"建立_关闭"时间戳改名
void Recorder::FinalizeFile(std::fstream& ofs, const std::filesystem::path& filepath) {
  ofs.flush();
  ofs.close();

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(filepath, ec);  // 不存在或不可访问时 ec 非零
  if (ec) {
    return;
  }
  if (size == 0) {  // 空段不留文件
    std::filesystem::remove(filepath, ec);
    return;
  }

  std::filesystem::rename(filepath, FinalizedFilename(), ec);  // 改名失败就留着过渡名，数据还在
}

/// @brief 关当前段，开下一段
void Recorder::RotateFile(std::fstream& ofs, const std::filesystem::path& old_filepath) {
  FinalizeFile(ofs, old_filepath);

  AllocateSegmentName();
  ofs.clear();  // 复位流状态才能重开
  ofs.open(current_filename_, std::ios::binary | std::ios::app);
}

/// @brief 取建立时间戳，挑一个没被占用的过渡文件名
void Recorder::AllocateSegmentName() {
  create_time_ = Utils::GetTimestamp();
  std::string name{TransitionalFilename()};
  // 定型名 = 过渡名 + 关闭时间戳 + 扩展名，所以过渡名不重就够；有残留同名文件时加序号
  std::error_code ec;
  for (std::size_t i{1}; std::filesystem::exists(name, ec); ++i) {
    name = TransitionalFilename() + "_" + std::to_string(i);
  }
  current_filename_ = std::move(name);
}

std::string Recorder::TransitionalFilename() const { return basic_filename_ + "_" + create_time_; }

std::string Recorder::FinalizedFilename() const { return TransitionalFilename() + "_" + Utils::GetTimestamp() + ext_; }

/// @brief 累计一次文件打开失败，返回是否已达重试上限
bool Recorder::ExceedFileOpenRetryLimit() { return ++file_open_retry_times_ >= kFileOpenRetryTimesLimit; }

}  // namespace jojo::record2
