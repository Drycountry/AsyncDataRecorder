# AsyncDataRecorder

一个基于 C++17 的轻量级异步二进制数据记录器。调用线程将数据提交到内存缓冲区，由后台线程批量写入文件，适合需要将数据采集与文件 I/O 解耦的场景。

## 功能特性

- **异步写入**：`Save()` 将数据复制到内部缓冲区，文件 I/O 在后台线程执行。
- **批量落盘**：满缓冲块累计到一定数量时触发写入；没有新数据时，后台线程也会定期检查并提交当前缓冲区。
- **有界待写队列**：限制满缓冲队列长度，避免写入速度跟不上时队列无限增长；可通过接口查询累计丢弃字节数。
- **文件分段与轮转**：每段文件关闭时使用创建和关闭时间戳命名；文件达到约 2 GiB 后开始新文件段。
- **关闭时收尾**：记录器析构时停止接收数据，并等待后台写线程退出。
- **并发访问**：`RecorderManager` 可按 ID 获取共享的记录器实例。

## 环境要求

- C++17 或更新的 C++ 编译器
- CMake 3.20 或更新版本

## 构建

在项目根目录执行：

```bash
cmake -S . -B build
cmake --build build --config Release
```

运行示例程序：

```bash
# Windows（Visual Studio 多配置生成器）
.\build\Release\AsyncDataRecorder.exe

# Linux / macOS
./build/AsyncDataRecorder
```

示例会在当前工作目录生成以记录器 ID 和时间戳命名的 `.dat` 文件。

## 使用示例

```cpp
#include <cstdint>

#include "recorder.hpp"

int main() {
  jojo::record2::Recorder recorder(
      "session", ".dat", jojo::record2::BcdEnable::kDisable);

  const std::uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  const bool accepted = recorder.Save(payload, sizeof(payload));

  // recorder 析构时会停止接收数据，并等待后台写线程结束。
  return accepted ? 0 : 1;
}
```

记录器将数据**原样写入二进制文件**，不会添加记录分隔符或转换数据格式。数据按块写入文件，不保证每次 `Save()` 对应一个独立文件记录。

### 写入状态与丢弃统计

```cpp
const auto state = recorder.GetWriteState();
const std::size_t dropped = recorder.GetDroppedBytes();
```

`GetWriteState()` 返回以下状态：

- `WriteState::kOk`：文件可写。
- `WriteState::kRetrying`：文件暂时不可用，正在重试。
- `WriteState::kFatal`：文件打开重试达到上限，记录器已停止接收数据。
- `WriteState::kStopped`：写线程已停止。

`Save()` 在参数无效、记录器停止接收、单次数据超过内部缓冲区容量，或队列满导致缓冲块被丢弃时返回 `false`。队列满时被丢弃的可能是此前缓冲的数据；如需监控数据损失，请同时检查 `GetDroppedBytes()`。该统计为尽力而为，底层文件写入失败时无法保证精确。

## 文件命名

已关闭的文件段命名格式为：

```text
<基础文件名>_<创建时间>_<关闭时间><扩展名>
```

例如：

```text
session_20261015T104827.321_20261015T104832.456.dat
```

时间戳使用本地时间，精确到毫秒。写入期间使用不带扩展名的临时文件名；正常关闭后，非空文件会重命名为上述格式。

## 项目结构

```text
.
├── CMakeLists.txt
├── main.cpp                 # 示例程序入口
├── recorder.hpp/.cpp        # 异步记录器
├── recorder_manager.hpp/.cpp# 按 ID 管理记录器
├── fixed_buffer.hpp         # 固定大小缓冲区
└── utils.hpp/.cpp           # 时间戳工具
```
