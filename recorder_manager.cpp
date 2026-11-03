#include "recorder_manager.hpp"

#include <stdexcept>
#include <utility>

namespace jojo::record2 {

namespace {

bool IsValidRecorderId(const std::string_view id) {
  if (id.empty() || id == "." || id == "..") {
    return false;
  }
  if (id.back() == '.' || id.back() == ' ') {
    return false;
  }

  constexpr std::string_view kInvalidChars{"<>:\"/\\|?*"};
  for (const unsigned char ch : id) {
    if (ch < 0x20 || ch == 0x7F || kInvalidChars.find(static_cast<char>(ch)) != std::string_view::npos) {
      return false;
    }
  }
  return true;
}

}  // namespace

RecorderManager::RecorderManager(RecorderOptions options) : options_{std::move(options)} {
  if (options_.output_directory.empty()) {
    options_.output_directory = ".";
  }
  if (options_.extension.empty() || options_.extension.front() != '.' ||
      options_.extension.find_first_of("/\\") != std::string::npos) {
    throw std::invalid_argument("recorder extension must start with '.' and contain no path separators");
  }

  std::error_code ec;
  std::filesystem::create_directories(options_.output_directory, ec);
  if (ec) {
    throw std::filesystem::filesystem_error("cannot create recorder output directory", options_.output_directory, ec);
  }
  if (std::filesystem::is_directory(options_.output_directory, ec) == false || ec) {
    throw std::filesystem::filesystem_error("recorder output path is not a directory", options_.output_directory, ec);
  }
}

RecorderManager::~RecorderManager() { CloseAllRecorders(); }

std::shared_ptr<Recorder> RecorderManager::CreateRecorder(const std::string_view id) {
  if (IsValidRecorderId(id) == false) {
    throw std::invalid_argument("recorder id must be a valid file stem");
  }

  std::string key{id};
  std::shared_ptr<Recorder> out;
  {
    std::scoped_lock lock{lock_recorders_};
    auto found = recorders_.find(key);
    if (found != recorders_.cend()) {
      out = found->second;
    } else {
      const std::filesystem::path file_stem = options_.output_directory / key;
      out = std::make_shared<Recorder>(file_stem.string(), options_.extension);
      recorders_.emplace(std::move(key), out);
    }
  }

  return out;
}

std::shared_ptr<Recorder> RecorderManager::FindRecorder(const std::string_view id) const {
  if (IsValidRecorderId(id) == false) {
    return nullptr;
  }

  std::scoped_lock lock{lock_recorders_};
  const auto found = recorders_.find(std::string{id});
  return found == recorders_.cend() ? nullptr : found->second;
}

bool RecorderManager::CloseRecorder(const std::string_view id) {
  if (IsValidRecorderId(id) == false) {
    return false;
  }

  std::shared_ptr<Recorder> recorder;
  {
    std::scoped_lock lock{lock_recorders_};
    auto found = recorders_.find(std::string{id});
    if (found == recorders_.end()) {
      return false;
    }
    recorder = std::move(found->second);
    recorders_.erase(found);
  }

  // 在管理器锁外等待写线程结束，避免阻塞其他记录器的查询和创建。
  recorder->Close();
  return true;
}

std::size_t RecorderManager::CloseAllRecorders() {
  std::unordered_map<std::string, std::shared_ptr<Recorder>> recorders;
  {
    std::scoped_lock lock{lock_recorders_};
    recorders.swap(recorders_);
  }

  const std::size_t count = recorders.size();
  for (auto& item : recorders) {
    item.second->Close();
  }
  return count;
}

bool RecorderManager::ContainsRecorder(const std::string_view id) const { return FindRecorder(id) != nullptr; }

std::size_t RecorderManager::Size() const {
  std::scoped_lock lock{lock_recorders_};
  return recorders_.size();
}

}  // namespace jojo::record2
