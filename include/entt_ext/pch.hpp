// Precompiled header for entt_ext consumers.
// Use it through a one-line local header in the consuming project, e.g.
// pch/entt_ext_pch.hpp: `#pragma once` + `#include <entt_ext/pch.hpp>`,
// passed as cpp_pch, so the PCH file itself stays inside the consuming project.
// Contains only stable external dependencies that rarely change.
// Rebuild cost: one extra compilation per target when these headers update.
#pragma once

// EnTT — Entity Component System (~94K lines)
#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
#include <entt/entity/storage.hpp>
#include <entt/graph/adjacency_matrix.hpp>
#include <entt/core/type_info.hpp>

// Cereal — Serialization
#include <cereal/cereal.hpp>
#include <cereal/archives/portable_binary.hpp>

// Boost.Asio — Async I/O, coroutines, channels
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

// spdlog — Logging
#include <spdlog/spdlog.h>

// Standard library (commonly used across TUs)
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
