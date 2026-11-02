#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "recorder_manager.hpp"

int main() {
  jojo::record2::RecorderManager manager;
  auto recorder1 = manager.GetRecorder("1");
  auto recorder2 = manager.GetRecorder("2");

  std::vector<std::uint8_t> vec{1, 2, 3, 4, 5};

  recorder1->Save(vec.data(), vec.size());
  std::this_thread::sleep_for(std::chrono::seconds(10));

  std::string content{"我爱你中国"};
  recorder2->Save(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
}