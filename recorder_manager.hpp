#ifndef JOJO_RECORD2_RECORDER_MANAGER_HPP_
#define JOJO_RECORD2_RECORDER_MANAGER_HPP_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "recorder.hpp"

namespace jojo::record2 {
class RecorderManager {
 public:
  RecorderManager();
  ~RecorderManager();

  std::shared_ptr<Recorder> GetRecorder(const std::string& id);

 private:
  mutable std::mutex lock_recorders_;
  std::unordered_map<std::string, std::shared_ptr<Recorder>> recorders_{};
};
}  // namespace jojo::record2

#endif  // !JOJO_RECORD2_RECORDER_MANAGER_HPP_