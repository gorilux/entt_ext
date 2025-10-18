#include "entt_ext/ecs.hpp"

#include <functional>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace entt_ext {

#ifdef __EMSCRIPTEN__
using main_loop_callback = std::function<void()>;
auto invoke_main_loop_callback(void* callback) -> void {
  auto* cb = reinterpret_cast<main_loop_callback*>(callback);
  (*cb)();
}
#endif

ecs::ecs(std::string const& persistance_filename)
  : ecs{allocator_type{}, persistance_filename} {
}

ecs::ecs(const allocator_type& allocator, std::string const& persistance_filename)
  : registry_{allocator}
  , continuous_loader_{registry_} {
  load_state(persistance_filename);
}

ecs::ecs(ecs&& other) noexcept
  : registry_{std::move(other.registry_)}
  , global_entity_{std::move(other.global_entity_)}
  , continuous_loader_{std::move(other.continuous_loader_)} {
}

ecs& ecs::operator=(ecs&& other) noexcept {
  if (this != &other) {
    registry_          = std::move(other.registry_);
    global_entity_     = std::move(other.global_entity_);
    continuous_loader_ = std::move(other.continuous_loader_);
  }
  return *this;
}

ecs::entity_type ecs::create() {
  return registry_.create();
}

ecs::entity_type ecs::create(const entity_type hint) {
  return registry_.create(hint);
}

ecs::version_type ecs::destroy(const entity_type entt) {
  return registry_.destroy(entt);
}

ecs::version_type ecs::destroy(const entity_type entt, const version_type version) {
  return registry_.destroy(entt, version);
}

auto ecs::main_io_context() -> asio::io_context& {
  return main_io_context_;
}

auto ecs::main_io_context() const -> const asio::io_context& {
  return main_io_context_;
}

auto ecs::concurrent_io_context() -> asio::io_context& {
  return concurrent_io_context_;
}

void ecs::run(int timeout_ms, size_t concurrency) {
  auto& main_io_ctx = main_io_context();

#ifndef __EMSCRIPTEN__
  auto& concurrent_io_ctx = concurrent_io_context();
  auto  dummy_timer       = boost::asio::deadline_timer{concurrent_io_ctx};

  asio::signal_set signals(main_io_ctx, SIGINT, SIGTERM);
  signals.async_wait([this](auto error_code, auto) {
    asio::post(main_io_context(), [this]() {
      stop();
    });
  });

  dummy_timer.async_wait([](auto) {});

  sort<entt_ext::system>([](const auto& lhs, const auto& rhs) {
    return lhs.stage < rhs.stage;
  });

  // Spawn global channel processor
  asio::co_spawn(main_io_ctx, process_command_channel(), asio::detached);

  asio::co_spawn(main_io_ctx, run_update_loop(timeout_ms, concurrency), asio::detached);

  if (concurrency > 1) {
    auto& thread_pool = emplace<asio::thread_pool>(global_entity_, concurrency);
    for (auto i = 0u; i < concurrency; i++) {
      boost::asio::post(thread_pool, [&concurrent_io_ctx]() {
        concurrent_io_ctx.run();
      });
    }
  }

  save_state();

  main_io_ctx.run();
  signals.cancel();
  dummy_timer.cancel();
  remove<asio::thread_pool>(global_entity_);

  clear();

#else // __EMSCRIPTEN__

  std::cout << "Starting" << std::endl;

  try {
    auto except_hander = [&](std::exception_ptr error) {
      if (error) {
        std::cout << "main error" << std::endl;
      }
    };

    std::unique_ptr<main_loop_callback> main_loop_cb(new main_loop_callback([&]() {
      asio::co_spawn(main_io_ctx, run_update_loop(20, 4), except_hander);
      auto pending_tasks = main_io_ctx.poll();
      if (pending_tasks == 0) {
        std::cout << "Restarting" << std::endl;
        main_io_ctx.restart();
      }
    }));

    emscripten_set_main_loop_arg(invoke_main_loop_callback, reinterpret_cast<void*>(main_loop_cb.get()), 0, true);
  } catch (std::exception const& e) {
    std::cout << "Exception: " << e.what() << std::endl;
  } catch (...) {
    std::cout << "Unknown Exception" << std::endl;
  }

#endif // !__EMSCRIPTEN__
}

auto ecs::run_update_loop(int timeout_ms, size_t concurrency) -> asio::awaitable<void> {

  auto executor = co_await asio::this_coro::executor;
  auto timer    = boost::asio::deadline_timer{executor};

  auto outstanding_systems = view<entt_ext::system>(entt::exclude<running<system_tag>, run_once_tag>);

  auto wait_for_systems_to_finish = [&]() -> asio::awaitable<void> {
    auto systems_running = view<running<system_tag>>();
    auto each_running    = view<running<each_tag>>();

    while (systems_running.begin() != systems_running.end()) {
      timer.expires_from_now(boost::posix_time::milliseconds(timeout_ms));
      co_await timer.async_wait(use_nothrow_awaitable);
      // spdlog::debug("waiting for systems to finish");
    }

    while (each_running.begin() != each_running.end()) {
      timer.expires_from_now(boost::posix_time::milliseconds(timeout_ms));
      co_await timer.async_wait(use_nothrow_awaitable);
      // spdlog::debug("waiting for parallel each to finish");
    }
    co_return;
  };

  while (running_) {
    timer.expires_from_now(boost::posix_time::milliseconds(timeout_ms));

    auto last_stage = stage::val::input;
    for (auto [entt, sys] : outstanding_systems.each()) {

      auto const diff_time = std::chrono::high_resolution_clock::now() - sys.last_invoke;
      auto const elapsed   = (std::chrono::duration_cast<std::chrono::milliseconds>(diff_time).count() / 1000.0);
      try {
        if (elapsed >= sys.interval && co_await sys.run(sys, *this, elapsed)) {
          sys.last_invoke    = std::chrono::high_resolution_clock::now();
          auto current_stage = static_cast<stage::val>((sys.stage / 1000) * 1000);
          if (current_stage != last_stage && defered_tasks_.size() > 0) {

            co_await wait_for_systems_to_finish();
            for (auto& task : defered_tasks_) {
              try {
                co_await task(*this);
              } catch (std::exception const& e) {
                //  spdlog::error("run_update_loop: {}", e.what());
              }
            }
            defered_tasks_.clear();
          }
          last_stage = current_stage;
        }

      } catch (std::exception const& e) {
        // spdlog::error("run_update_loop: {}", e.what());
        sys.last_invoke = std::chrono::high_resolution_clock::now();
      }
    }

    // Need to sync all systems before we can delete any entities
    auto entities_pending_delete = view<entt_ext::delete_later>();
    if (entities_pending_delete.begin() != entities_pending_delete.end() || defered_tasks_.size() > 0) {

      co_await wait_for_systems_to_finish();

      // spdlog::info("Running {} defered tasks", defered_tasks_.size());
      for (auto& task : defered_tasks_) {
        try {
          co_await task(*this);
        } catch (std::exception const& e) {
          //  spdlog::error("run_update_loop: {}", e.what());
        }
      }
      defered_tasks_.clear();

      // spdlog::info("Deleting {} entities", entities_pending_delete.size());
      for (auto [delete_entity] : entities_pending_delete.each()) {
        destroy(delete_entity);
        // spdlog::info("Deleted {}", static_cast<int>(delete_entity));
      }
    }

    if (!running_) {
      // spdlog::info("Stopping update loop");
      co_await wait_for_systems_to_finish();
      asio::post(main_io_context(), [this]() {
        // spdlog::info("Stopping concurrent io context");
        concurrent_io_context().stop();
      });
      asio::post(main_io_context(), [this]() {
        // spdlog::info("Main io context stopped");
        main_io_context().stop();
      });
      // spdlog::info("All systems finished");
      co_return;
    }

    auto [ec] = co_await timer.async_wait(use_nothrow_awaitable);

    if (ec) {
      // spdlog::warn("run_update_loop: {}", ec.message());
      co_return;
    }
  }
  co_return;
}

void ecs::stop() {
  running_ = false;
}

// ============= Deferred Operations Implementation =============

// Channel processor coroutine - runs on main executor
asio::awaitable<void> ecs::process_command_channel() {
  while (running_) {
    auto [ec, cmd] = co_await command_channel_.async_receive(asio::as_tuple(asio::use_awaitable));

    if (ec) {
      if (ec == asio::error::operation_aborted || ec == asio::experimental::error::channel_closed) {
        break; // Channel closed, exit gracefully
      }
      // Handle other errors
      continue;
    }

    // Execute command on main executor
    try {
      co_await cmd.executor(*this);
    } catch (std::exception const& e) {
      // Log error if needed, but continue processing
      // spdlog::error("Error executing deferred command: {}", e.what());
    }
  }
}

// Legacy deferred operations (simplified - channels handle everything now)
ecs::entity_type ecs::create_deferred() {
  // Just create immediately - no context tracking needed
  return create();
}

void ecs::save_state() {
  if (auto settings_filename_ptr = try_get<settings_filename>(global_entity_); settings_filename_ptr) {
    std::ofstream                       ofs(settings_filename_ptr->filename, std::ios::binary);
    cereal::PortableBinaryOutputArchive ar(ofs);
    entt::snapshot{registry_}.get<entt_ext::entity>(ar).template get<state_persistance_header>(ar).template get<main_context_tag>(ar);
  }
}

void ecs::load_state(std::string const& filename) {
  using InputArchiveType = cereal::PortableBinaryInputArchive;

  if (!filename.empty()) {
    try {
      std::ifstream    ifs(filename, std::ios::binary);
      InputArchiveType ar(ifs);
      // entt::snapshot_loader{registry_}
      continuous_loader_.get<entt_ext::entity>(ar).template get<state_persistance_header>(ar).template get<main_context_tag>(ar);
    } catch (std::exception const& ex) {
      throw;
    }

    for (auto [entt] : view<main_context_tag>().each()) {
      global_entity_ = entt;
      break;
    }
  }

  if (global_entity_ == entt_ext::null) {
    global_entity_ = create();
    emplace<state_persistance_header>(global_entity_);
    emplace<main_context_tag>(global_entity_);
  }

  if (!filename.empty()) {
    emplace<settings_filename>(global_entity_, filename);
  }
}

} // namespace entt_ext
