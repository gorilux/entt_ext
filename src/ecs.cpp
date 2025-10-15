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
  : m_registry{allocator}
  , m_continuous_loader{m_registry} {
  load_state(persistance_filename);
}

ecs::ecs(ecs&& other) noexcept
  : m_registry{std::move(other.m_registry)}
  , m_global_entity{std::move(other.m_global_entity)}
  , m_concurrent_entity(other.m_concurrent_entity)
  , m_continuous_loader{std::move(other.m_continuous_loader)} {
}

ecs& ecs::operator=(ecs&& other) noexcept {
  if (this != &other) {
    m_registry          = std::move(other.m_registry);
    m_global_entity     = std::move(other.m_global_entity);
    m_concurrent_entity = std::move(other.m_concurrent_entity);
    m_continuous_loader = std::move(other.m_continuous_loader);
  }
  return *this;
}

ecs::entity_type ecs::create() {
  return m_registry.create();
}

ecs::entity_type ecs::create(const entity_type hint) {
  return m_registry.create(hint);
}

ecs::version_type ecs::destroy(const entity_type entt) {
  return m_registry.destroy(entt);
}

ecs::version_type ecs::destroy(const entity_type entt, const version_type version) {
  return m_registry.destroy(entt, version);
}

auto ecs::main_io_context() -> asio::io_context& {
  return get<asio::io_context>(m_global_entity);
}

auto ecs::concurrent_io_context() -> asio::io_context& {
  return get<asio::io_context>(m_concurrent_entity);
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

  asio::co_spawn(main_io_ctx, run_update_loop(timeout_ms, concurrency), asio::detached);

  if (concurrency > 1) {
    auto& thread_pool = emplace<asio::thread_pool>(m_concurrent_entity, concurrency);
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
  remove<asio::thread_pool>(m_concurrent_entity);

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

  while (m_running) {
    timer.expires_from_now(boost::posix_time::milliseconds(timeout_ms));

    auto last_stage = stage::val::input;
    for (auto [entt, sys] : outstanding_systems.each()) {

      auto const diff_time = std::chrono::high_resolution_clock::now() - sys.last_invoke;
      auto const elapsed   = (std::chrono::duration_cast<std::chrono::milliseconds>(diff_time).count() / 1000.0);
      try {
        if (elapsed >= sys.interval && co_await sys.run(sys, *this, elapsed)) {
          sys.last_invoke    = std::chrono::high_resolution_clock::now();
          auto current_stage = static_cast<stage::val>((sys.stage / 1000) * 1000);
          if (current_stage != last_stage && m_defered_tasks.size() > 0) {

            co_await wait_for_systems_to_finish();
            for (auto& task : m_defered_tasks) {
              try {
                co_await task(*this);
              } catch (std::exception const& e) {
                //  spdlog::error("run_update_loop: {}", e.what());
              }
            }
            m_defered_tasks.clear();
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
    if (entities_pending_delete.begin() != entities_pending_delete.end() || m_defered_tasks.size() > 0) {

      co_await wait_for_systems_to_finish();

      // spdlog::info("Running {} defered tasks", m_defered_tasks.size());
      for (auto& task : m_defered_tasks) {
        try {
          co_await task(*this);
        } catch (std::exception const& e) {
          //  spdlog::error("run_update_loop: {}", e.what());
        }
      }
      m_defered_tasks.clear();

      // spdlog::info("Deleting {} entities", entities_pending_delete.size());
      for (auto [delete_entity] : entities_pending_delete.each()) {
        destroy(delete_entity);
        // spdlog::info("Deleted {}", static_cast<int>(delete_entity));
      }
    }

    if (!m_running) {
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
  m_running = false;
}

void ecs::save_state() {
  if (auto settings_filename_ptr = try_get<settings_filename>(m_global_entity); settings_filename_ptr) {
    std::ofstream                       ofs(settings_filename_ptr->filename, std::ios::binary);
    cereal::PortableBinaryOutputArchive ar(ofs);
    entt::snapshot{m_registry}
        .get<entt_ext::entity>(ar)
        .template get<state_persistance_header>(ar)
        .template get<main_context_tag>(ar)
        .template get<concurrent_context_tag>(ar);
  }
}

void ecs::load_state(std::string const& filename) {
  using InputArchiveType = cereal::PortableBinaryInputArchive;

  if (!filename.empty()) {
    try {
      std::ifstream    ifs(filename, std::ios::binary);
      InputArchiveType ar(ifs);
      // entt::snapshot_loader{m_registry}
      m_continuous_loader.get<entt_ext::entity>(ar)
          .template get<state_persistance_header>(ar)
          .template get<main_context_tag>(ar)
          .template get<concurrent_context_tag>(ar);
    } catch (std::exception const& ex) {
      throw;
    }

    for (auto [entt] : view<main_context_tag>().each()) {
      m_global_entity = entt;
      break;
    }
    for (auto [entt] : view<concurrent_context_tag>().each()) {
      m_concurrent_entity = entt;
      break;
    }
  }

  if (m_global_entity == entt_ext::null) {
    m_global_entity = create();
    emplace<state_persistance_header>(m_global_entity);
    emplace<main_context_tag>(m_global_entity);
  }

  if (m_concurrent_entity == entt_ext::null) {
    m_concurrent_entity = create();
    emplace<concurrent_context_tag>(m_concurrent_entity);
  }

  if (!filename.empty()) {
    emplace<settings_filename>(m_global_entity, filename);
  }

  emplace<asio::io_context>(m_global_entity);
  emplace<asio::io_context>(m_concurrent_entity);
}

} // namespace entt_ext
