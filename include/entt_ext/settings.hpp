#pragma once

#include <entt_ext/ecs_fwd.hpp>

#include <spdlog/spdlog.h>

#include <cereal/archives/json.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <fstream>
#include <functional>

namespace entt_ext {
struct settings_header {

  std::uint32_t version     = 0;
  std::string   app_version = "0.0.0";

  template <typename ArchiveT>
  void serialize(ArchiveT& ar, std::uint32_t const class_version) {
    if (class_version > 0) {
      ar(CEREAL_NVP(version));
      ar(CEREAL_NVP(app_version));
    }
  }
};
} // namespace entt_ext

CEREAL_CLASS_VERSION(entt_ext::settings_header, 1);

namespace entt_ext {
namespace asio = boost::asio;

template <typename... ComponentsT>
struct persistent_data_model {

  template <typename ArchiveT>
  static asio::awaitable<void> load(ArchiveT& archive, entt_ext::ecs& ecs) {
    co_await ecs.load_snapshot<ArchiveT, ComponentsT...>(archive);
  }

  template <typename ArchiveT>
  static asio::awaitable<void> save(ArchiveT& archive, entt_ext::ecs const& ecs) {
    co_await ecs.save_snapshot<ArchiveT, ComponentsT...>(archive);
  }

  template <typename ArchiveT>
  static asio::awaitable<bool> merge(ArchiveT& archive, entt_ext::ecs& ecs) {
    co_return co_await ecs.merge_snapshot<ArchiveT, ComponentsT...>(archive);
  }
};

template <typename... ComponentsT>
struct settings_graph {

  template <typename ArchiveT>
  static asio::awaitable<void> load(ArchiveT& archive, entt_ext::ecs& ecs) {
    co_await ecs.load_graph_snapshot<ArchiveT, ComponentsT...>(archive);
  }

  template <typename ArchiveT>
  static asio::awaitable<void> save(ArchiveT& archive, entt_ext::ecs const& ecs) {
    co_await ecs.save_graph_snapshot<ArchiveT, ComponentsT...>(archive);
  }

  template <typename ArchiveT>
  static asio::awaitable<bool> merge(ArchiveT& archive, entt_ext::ecs& ecs) {
    co_return co_await ecs.merge_graph_snapshot<ArchiveT, ComponentsT...>(archive);
  }
};

template <int base_version, typename... SettingsT>
struct settings_collection {

  constexpr static int latest_version = sizeof...(SettingsT) + base_version - 1;

  template <typename ArchiveT>
  static asio::awaitable<void> load(int version, ArchiveT& archive, entt_ext::ecs& ecs) {

    using fptr = asio::awaitable<void> (*)(ArchiveT&, entt_ext::ecs&);

    static constexpr fptr table[] = {SettingsT::load...};
    co_await table[version - base_version](archive, ecs);

    co_return;
  }

  template <typename ArchiveT>
  static asio::awaitable<void> save(int version, ArchiveT& archive, entt_ext::ecs const& ecs) {

    using fptr = asio::awaitable<void> (*)(ArchiveT&, entt_ext::ecs const&);

    static constexpr fptr table[] = {SettingsT::save...};
    co_await table[version - base_version](archive, ecs);

    co_return;
  }

  template <typename ArchiveT>
  static asio::awaitable<bool> merge(int version, ArchiveT& archive, entt_ext::ecs& ecs) {

    using fptr = asio::awaitable<bool> (*)(ArchiveT&, entt_ext::ecs&);

    static constexpr fptr table[] = {SettingsT::merge...};

    co_return co_await table[version - base_version](archive, ecs);
  }
};

// using InputArchiveType  = cereal::JSONInputArchive;
// using OutputArchiveType = cereal::JSONOutputArchive;

using InputArchiveType  = cereal::PortableBinaryInputArchive;
using OutputArchiveType = cereal::PortableBinaryOutputArchive;

template <typename SettingsCollectionT>
asio::awaitable<bool> save(std::string const& filename, entt_ext::ecs const& ecs) {

  std::ofstream     ofs(filename, std::ios::binary);
  OutputArchiveType archive(ofs);
  settings_header   header{.version = SettingsCollectionT::latest_version};
  archive(header);

  co_await SettingsCollectionT::save(header.version, archive, ecs);
  co_return true;
}

template <typename SettingsCollectionT>
asio::awaitable<entt_ext::ecs> load(std::string const& filename) {

  entt_ext::ecs ecs;
  if (!std::filesystem::exists(filename)) {
    co_return ecs;
  }

  std::ifstream    ifs(filename, std::ios::binary);
  InputArchiveType archive(ifs);
  settings_header  header;
  archive(header);

  co_await SettingsCollectionT::load(header.version, archive, ecs);

  co_return ecs;
}

template <typename SettingsCollectionT>
asio::awaitable<bool> merge(std::string const& filename, entt_ext::ecs& ecs) {

  if (!std::filesystem::exists(filename)) {
    co_return false;
  }

  std::ifstream    ifs(filename, std::ios::binary);
  InputArchiveType archive(ifs);
  settings_header  header;
  archive(header);

  co_return co_await SettingsCollectionT::merge(header.version, archive, ecs);
}
} // namespace entt_ext