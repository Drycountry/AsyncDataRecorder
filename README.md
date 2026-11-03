# AsyncBinaryRecorder

AsyncBinaryRecorder 是一个轻量级的 C++ 二进制数据记录工具。

它可以在采集数据时快速接收内容，由后台自动写入磁盘，减少文件读写对主流程的影响。它适合持续采集的传感器数据、实时运行日志、设备采样结果等二进制数据归档场景。

## 主要特点

- **录制简单**：传入数据内容和长度即可保存。
- **后台写入**：数据采集与磁盘写入互不干扰，适合长时间持续录制。
- **自动分段**：文件变大或记录结束时自动生成独立文件，便于后续传输和查看。
- **数据保真**：写入内容与原数据一致，不会修改或重新编码。
- **多任务记录**：可以按不同 ID 同时管理多个记录任务。

## 快速开始

环境要求：支持 C++17 的编译器，以及 CMake 3.20 或更新版本。项目无需额外安装第三方组件。

```bash
cmake -S . -B build
cmake --build build --config Release
```

构建后运行示例程序即可生成演示录制文件：

```bash
# Windows
.\build\Release\AsyncBinaryRecorder.exe

# Linux / macOS
./build/AsyncBinaryRecorder
```

## 基本用法

```cpp
#include <cstdint>
#include <string>

#include "recorder.hpp"

int main() {
  jojo::record2::Recorder recorder("session", ".dat");

  const std::string message = "Hello, AsyncBinaryRecorder!";
  const auto* data =
      reinterpret_cast<const std::uint8_t*>(message.data());

  recorder.Save(data, message.size());
}
```

`Save()` 返回 `false` 时，表示本次数据未被接收，或已有数据未能安全保存。长时间录制时，建议检查返回值并处理异常情况。

## 录制文件

录制文件默认以 `.dat` 结尾，文件名包含记录器名称和生成时间。每次录制结束后会保留一个完整文件，不会覆盖已有文件。

文件内容是原始二进制数据，适合由读取程序按预先约定的格式解析。
