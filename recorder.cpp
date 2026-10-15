#include "recorder.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "utils.hpp"

/// @brief 匿名命名空间，仅在当前编译单元有效
namespace {
// 满缓存队列长度 ≥kQueueSizeLimit 落盘 1 次
constexpr static inline std::size_t kQueueSizeNotifyLimit{5};

// 每隔 5s 落盘 1 次
constexpr static inline std::chrono::seconds kTimeLimit{5};

// 文件打开失败重试上限，达到后放弃落盘（写线程进入 kFatal）
constexpr static inline int kFileOpenRetryTimesLimit{10};

// 文件体积上限，超上限则滚动到下一文件继续记录
constexpr static inline std::size_t kMaxFileSize{2147483648};  // 2GB
}  // namespace

namespace jojo::record2 {

/// @brief 构造函数
Recorder::Recorder(const std::string& basic_filename, const std::string& ext, BcdEnable bcd_enable)
    : basic_filename_{basic_filename}, ext_{ext}, bcd_enable_{bcd_enable} {
  // 满缓存队列长度 ≥kQueueSizeLimit 落盘 1
  // 次，所以总缓存数应大于kQueueSizeLimit，保证总有空闲队列可用
  for (int i{}; i < kQueueSizeNotifyLimit + 1; ++i) {
    PushToFreeQueue(AllocateBuffer());
  }
  current_buffer_ = PopFromFreeQueue();
  StartSegment();

  // 初始化线程
  thread_ = std::thread([this]() {
    this->Write();

    // 线程退出前清空队列、缓存，更新 dropped_bytes_
    {
      std::scoped_lock lock{lock_current_buffer_};
      if (current_buffer_ != nullptr) {
        dropped_bytes_ += current_buffer_->Length();
      }
      current_buffer_ = nullptr;
    }
    {
      std::scoped_lock lock{lock_free_queue_};
      free_queue_.clear();  // 清除元素，但队列内存不会释放
    }
    {
      std::scoped_lock lock{lock_full_queue_};
      while (full_queue_.empty() == false) {
        auto buffer = std::move(full_queue_.front());
        full_queue_.pop_front();
        if (buffer != nullptr) {
          dropped_bytes_ += buffer->Length();
        }
      }
    }
  });
}

/// @brief 析构函数
Recorder::~Recorder() {
  // 当前缓存入队，保证所有缓存数据能够写入文件
  std::unique_ptr<BufferType> pending{};
  {
    std::scoped_lock lock{lock_current_buffer_};
    pending = std::move(current_buffer_);
  }
  PushToFullQueueForced(std::move(pending));  // 最后一段不受有界队列限流：写线程必定会排空它

  // 不再接收数据
  accepting_.store(false, std::memory_order_release);
  cv_write_file_.notify_one();
  thread_.join();
}

/// @brief 保存数据，由外部调用
/// @param data 地址（不应为nullptr）
/// @param length 长度（＞0）
/// @return 是否已接收；false 表示未接收（析构中/已停止接收/超长）或 `length==0` 或 `data==nullptr`
///         或入队时丢弃了某个已满缓存（丢弃量由 `GetDroppedBytes()`获取）
bool Recorder::Save(const std::uint8_t* data, const std::size_t length) {
  if (data == nullptr || length == 0) {
    return false;
  }

  // 对象析构中，或写线程已停止（kStopped/kFatal）：不再接收数据
  const WriteState state = write_state_.load(std::memory_order_acquire);
  if (accepting_.load(std::memory_order_acquire) == false || state == WriteState::kFatal ||
      state == WriteState::kStopped) {
    dropped_bytes_ += length;
    return false;
  }

  // 超过缓存容量上限：不接收数据
  if (length > kBufferSize) {
    dropped_bytes_ += length;
    return false;
  }

  // 用于临时存放 current_buffer_ 内容，供写入 full_queue_，避免 current_buffer_、full_queue_ 的对应锁同时上锁（易死锁）
  std::unique_ptr<BufferType> pending_to_full_queue{};
  {
    std::scoped_lock lock{lock_current_buffer_};
    if (current_buffer_ == nullptr) {
      current_buffer_ = std::move(PopFromFreeQueue());
    }
    if (current_buffer_->Avail() <= length) {  // 当前缓冲满，推入满队列待落盘
      pending_to_full_queue = std::move(current_buffer_);
      current_buffer_ = std::move(PopFromFreeQueue());
    }
    current_buffer_->Append(data, length);
  }

  // 缓存入队
  if (pending_to_full_queue != nullptr) {
    return PushToFullQueue(std::move(pending_to_full_queue));
  }

  // 没有缓存需要入队
  return true;
}

/// @brief 数据落盘
void Recorder::Write() {
  // 局部队列，缓存空闲缓存
  std::deque<std::unique_ptr<BufferType>> local_free_queue;
  // 本批次待写入的满缓存，锁内与 full_queue_ 交换得到，落盘在锁外进行
  std::deque<std::unique_ptr<BufferType>> batch;

  // 丢弃批次中剩余数据，并计入未落盘字节数
  auto drop_batch = [this, &batch]() {
    while (batch.empty() == false) {
      if (batch.front() != nullptr) {
        dropped_bytes_ += batch.front()->Length();
      }
      batch.pop_front();
    }
  };

  // 保持文件大部分时刻处于状态，在线程退出、轮转期间处于关闭状态
  std::fstream ofs(current_filename_, std::ios::binary | std::ios::app);

  while (true) {
    // 文件不可用：退避重试，达到上限则放弃
    if (ofs.is_open() == false) {
      if (accepting_.load(std::memory_order_acquire) == false) {
        break;  // 关机：当前段文件不存在，无需定型
      }
      if (ExceedFileOpenRetryLimit() == true) {
        write_state_.store(WriteState::kFatal, std::memory_order_release);  // 停止接收数据
        break;
      }
      write_state_.store(WriteState::kRetrying, std::memory_order_release);  // 文件暂不可用
      std::this_thread::sleep_for(std::chrono::milliseconds(100));           // 避免忙等
      ofs.clear();
      ofs.open(current_filename_, std::ios::binary | std::ios::app);
      continue;  // 重新判断
    }
    file_open_retry_times_ = 0;  // 文件可用：清零打开失败计数
    write_state_.store(WriteState::kOk, std::memory_order_release);

    // 只持锁等待与取走待写数据，落盘在锁外进行
    bool finished{};
    {
      std::unique_lock lk{lock_full_queue_};
      // cv_signaled: true 正常触发未超时，false 超时
      const bool cv_signaled = cv_write_file_.wait_for(lk, std::chrono::seconds(kTimeLimit), [this]() -> bool {
        return (full_queue_.empty() == false)                             // 队列非空
               || (accepting_.load(std::memory_order_acquire) == false);  // 停止接收数据，对象正在析构
      });
      lk.unlock();  // 提交当前缓存需另取 lock_current_buffer_，先放开本锁避免锁嵌套

      if (cv_signaled == false) {  // 条件变量等待超时，当前缓存交付落盘
        std::unique_ptr<BufferType> pending;
        {
          std::scoped_lock lock{lock_current_buffer_};
          pending = std::move(current_buffer_);
        }
        PushToFullQueue(std::move(pending));
      }

      lk.lock();  // 取走队列与判定退出须在同一临界区（否则会漏掉析构时推入的最后一段）
      if (batch.empty() == true) {
        batch.swap(full_queue_);  // 锁内只做 O(1) 交换
      } else {                    // 上批有残留（写失败退出）：追加，保证旧数据在前
        while (full_queue_.empty() == false) {
          batch.push_back(std::move(full_queue_.front()));
          full_queue_.pop_front();
        }
      }
      if (batch.empty() == true && accepting_.load(std::memory_order_acquire) == false) {
        finished = true;  // 已停止接收且无待写数据，退出
      }
    }

    if (batch.empty() == true) {  // 没得可写，下一循环等待
      if (finished == true) {
        break;
      }
      continue;
    }

    // 锁外落盘
    std::filesystem::path filepath{current_filename_};
    std::error_code ec;
    const std::uintmax_t exist_size = std::filesystem::file_size(filepath, ec);  // 不存在或不可访问时 ec 非零
    std::size_t filesize = ec ? 0 : static_cast<std::size_t>(exist_size);
    std::size_t batch_bytes = 0;  // 本批次已交给流的字节数，用于 flush 失败时统计未落盘字节

    while (batch.empty() == false) {  // 写空批次，全部写入文件
      auto buffer = std::move(batch.front());
      batch.pop_front();
      if (buffer == nullptr) {
        continue;
      }

      ofs.write(reinterpret_cast<const char*>(buffer->Data()), buffer->Length());
      if (ofs.good() == false) {  // 写失败
        dropped_bytes_ += buffer->Length();
        write_state_.store(WriteState::kRetrying, std::memory_order_release);  // 文件暂不可用
        ofs.clear();  // 先复位状态，尽量把已接收的数据刷出，再关闭重开
        ofs.flush();
        ofs.close();
        break;  // 回到循环开头退避重试，批次内剩余数据留待下一轮
      }
      filesize += buffer->Length();
      batch_bytes += buffer->Length();

      // 文件体积超上限，触发文件轮转
      if (filesize >= kMaxFileSize) {
        RotateFile(ofs, filepath);
        filesize = 0;                  // 新的文件，体积清零
        filepath = current_filename_;  // 新的文件路径

        if (ofs.is_open() == false) {                                            // 重开失败：回到循环开头退避重试
          write_state_.store(WriteState::kRetrying, std::memory_order_release);  // 文件暂不可用
          break;
        }
      }
      local_free_queue.push_back(std::move(buffer));
    }

    // 确实写入流，那么刷新IO缓存+缓存进入free_queue_
    if (local_free_queue.empty() == false) {
      if (ofs.is_open() == true) {
        ofs.flush();
        if (ofs.good() == false) {  // 刷盘失败：本批次数据未真正落盘
          dropped_bytes_ += batch_bytes;
          write_state_.store(WriteState::kRetrying, std::memory_order_release);  // 文件暂不可用
          ofs.close();
          ofs.clear();
        }
      }
      PushToFreeQueue(local_free_queue);
    }
  }

  // 正常关机退出置 kStopped（保留 kFatal：放弃落盘的状态不能被覆盖）
  if (write_state_.load(std::memory_order_acquire) != WriteState::kFatal) {
    write_state_.store(WriteState::kStopped, std::memory_order_release);
  }

  drop_batch();                          // 剩余批次数据无法落盘，计入未落盘字节数
  FinalizeFile(ofs, current_filename_);  // 关机：关闭文件并以关闭时间戳定型文件名
}

void Recorder::PushToFreeQueue(std::unique_ptr<BufferType>&& elem) {
  if (elem == nullptr) {
    return;
  }
  elem->Reset();
  {
    std::scoped_lock lock{lock_free_queue_};
    free_queue_.push_back(std::move(elem));
  }
}

void Recorder::PushToFreeQueue(std::deque<std::unique_ptr<BufferType>>& queue) {
  if (queue.empty() == true) {
    return;  // 避免加锁
  }

  std::scoped_lock lock{lock_free_queue_};
  while (queue.empty() == false) {
    auto buffer = std::move(queue.front());
    buffer->Reset();
    queue.pop_front();
    free_queue_.push_back(std::move(buffer));
  }
}

std::unique_ptr<BufferType> Recorder::PopFromFreeQueue() {
  {
    std::scoped_lock lock{lock_free_queue_};
    if (free_queue_.empty() == false) {
      auto elem = std::move(free_queue_.front());
      free_queue_.pop_front();
      return elem;
    }
  }
  return AllocateBuffer();
}

/// @brief 缓存入满缓存队列。若队列长度已超出预设上限，则丢弃新到来的缓存块并返回 false；否则返回 true
bool Recorder::PushToFullQueue(std::unique_ptr<BufferType>&& buffer) {
  if (buffer == nullptr) {
    return false;
  }

  // 若满缓存队列长度已经超限，那么将缓存清空并归还到空缓存队列
  {
    // 满缓存队列长度未超限：正常逻辑
    std::scoped_lock lock{lock_full_queue_};
    if (full_queue_.size() < kMaxFullQueueSize) {
      full_queue_.push_back(std::move(buffer));
      if (full_queue_.size() >= kQueueSizeNotifyLimit) {
        cv_write_file_.notify_one();
      }
      return true;
    }
  }

  // full_queue_.size() >= kMaxFullQueueSize
  dropped_bytes_ += buffer->Length();
  buffer->Reset();
  PushToFreeQueue(std::move(buffer));
  return false;
}

/// @brief 满缓存队列入队（不受有界队列限制，供析构时交付最后一段数据）
void Recorder::PushToFullQueueForced(std::unique_ptr<BufferType>&& buffer) {
  if (buffer == nullptr) {
    return;
  }

  std::scoped_lock lock{lock_full_queue_};
  full_queue_.push_back(std::move(buffer));
  cv_write_file_.notify_one();
}

std::unique_ptr<BufferType> Recorder::AllocateBuffer() { return std::make_unique<BufferType>(); }

/// @brief 写线程状态，kFatal 表示已停止接收数据
WriteState Recorder::GetWriteState() const { return write_state_.load(std::memory_order_acquire); }

/// @brief 关闭文件并按"建立时间戳_关闭时间戳"定型文件名
/// @param ofs 文件流
/// @param filepath 待定型的文件路径
void Recorder::FinalizeFile(std::fstream& ofs, const std::filesystem::path& filepath) {
  ofs.flush();
  ofs.close();  // 刷新IO缓存，关闭文件

  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(filepath, ec);  // 不存在或不可访问时 ec 非零
  if (ec) {
    return;
  }
  if (size == 0) {  // 空段不留文件
    std::filesystem::remove(filepath, ec);
    return;
  }

  std::filesystem::rename(filepath, FilenameStage2(), ec);  // 失败时保留过渡名，数据仍在文件里
}

/// @brief 文件轮转
/// @param ofs 文件流
/// @param old_filepath 旧文件路径（用于重命名）
void Recorder::RotateFile(std::fstream& ofs, const std::filesystem::path& old_filepath) {
  FinalizeFile(ofs, old_filepath);  // 当前段定型：建立时间戳_关闭时间戳 + 扩展名

  StartSegment();  // 为下一文件段记录建立时间戳并分配文件名
  ofs.clear();     // 复位流状态后才能重新 open
  ofs.open(current_filename_, std::ios::binary | std::ios::app);
}

/// @brief 开始新文件段：记录文件建立时间戳，并分配未被占用的过渡文件名
void Recorder::StartSegment() {
  create_time_ = Utils::GetTimestamp();
  std::string name{FilenameStage1()};
  // 定型名 = 过渡名 + 关闭时间戳 + 扩展名，故过渡名唯一即可保证定型名唯一；
  // 存在同名残留文件（上次异常退出、同一毫秒重启等）时追加序号，避免覆盖已有数据
  std::error_code ec;
  for (std::size_t i{1}; std::filesystem::exists(name, ec); ++i) {
    name = FilenameStage1() + "_" + std::to_string(i);
  }
  current_filename_ = std::move(name);
}

std::string Recorder::FilenameStage1() const { return basic_filename_ + "_" + create_time_; }

std::string Recorder::FilenameStage2() const { return FilenameStage1() + "_" + Utils::GetTimestamp() + ext_; }

/// @brief 累计一次文件打开失败，返回是否已达重试上限
bool Recorder::ExceedFileOpenRetryLimit() { return ++file_open_retry_times_ >= kFileOpenRetryTimesLimit; }

}  // namespace jojo::record2