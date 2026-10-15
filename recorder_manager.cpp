#include "recorder_manager.hpp"

namespace jojo::record2 {

RecorderManager::RecorderManager() {}

RecorderManager::~RecorderManager() {}

std::shared_ptr<Recorder> RecorderManager::GetRecorder(const std::string& id) {
  std::shared_ptr<Recorder> out;
  {
    std::scoped_lock lock{lock_recorders_};
    auto found = recorders_.find(id);
    if (found != recorders_.cend()) {
      out = found->second;
    } else {
      out = std::make_shared<Recorder>(id, ".dat", BcdEnable::kEnable);
      recorders_.emplace(id, out);
    }
  }

  return out;
}
}  // namespace jojo::record2
