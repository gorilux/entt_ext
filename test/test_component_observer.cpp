// component_observer contract tests.
//
// These pin the behaviour sync_client depends on and that is otherwise only
// observable at runtime, over a socket, mid-snapshot:
//
//   * a sync observer body runs inline on the signalling call stack, an async
//     one is queued on the ECS command channel;
//   * ecs::set_async_observers_muted(true) drops async dispatches outright —
//     it does not buffer them, so unmuting replays nothing (this is exactly
//     why sync_client_impl.hpp keeps its own muted_update_replay_ list);
//   * an async body sees the component as of dispatch time, not as of the
//     moment the channel happens to drain it.
//
// The harness drives the main executor with ecs::poll() rather than ecs::run(),
// so everything below is single-threaded and deterministic.

#include <gtest/gtest.h>

#include <entt_ext/ecs.hpp>

#include <boost/asio/awaitable.hpp>

#include <vector>

namespace {

namespace asio = boost::asio;

// Plain copyable component: component_observer::snapshot_component only
// snapshots copy-constructible, paged types (move-only ones snapshot to null
// by design).
struct observed_value {
  int value = 0;
};

// Run the main executor until it goes quiet. A single poll() already runs
// handlers that become ready during the poll, but a drained command is free to
// defer another one, so loop until a round does nothing.
void pump(entt_ext::ecs& ecs) {
  for (int round = 0; round < 64; ++round) {
    if (ecs.poll() == 0) {
      return;
    }
  }
  ADD_FAILURE() << "pump: the main executor never went quiet after 64 rounds";
}

class component_observer_test : public ::testing::Test {
protected:
  entt_ext::ecs ecs_;
};

TEST_F(component_observer_test, sync_bodies_run_inline_and_async_bodies_are_deferred) {
  int sync_calls  = 0;
  int async_calls = 0;

  auto& observer = ecs_.component_observer<observed_value>();
  observer.on_construct([&](entt_ext::ecs&, entt_ext::entity, observed_value&) {
    ++sync_calls;
  });
  observer.on_construct([&](entt_ext::ecs&, entt_ext::entity, observed_value&) -> asio::awaitable<void> {
    ++async_calls;
    co_return;
  });

  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 1);

  EXPECT_EQ(sync_calls, 1);
  EXPECT_EQ(async_calls, 0) << "async observer bodies must go through the command channel, not run inline";

  pump(ecs_);
  EXPECT_EQ(async_calls, 1);
}

TEST_F(component_observer_test, muting_suppresses_async_bodies_but_not_sync_ones) {
  int sync_calls  = 0;
  int async_calls = 0;

  auto& observer = ecs_.component_observer<observed_value>();
  observer.on_construct([&](entt_ext::ecs&, entt_ext::entity, observed_value&) {
    ++sync_calls;
  });
  observer.on_construct([&](entt_ext::ecs&, entt_ext::entity, observed_value&) -> asio::awaitable<void> {
    ++async_calls;
    co_return;
  });

  ecs_.set_async_observers_muted(true);

  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 1);
  pump(ecs_);

  EXPECT_EQ(sync_calls, 1) << "sync observers must keep firing while muted — snapshot ingest relies on them";
  EXPECT_EQ(async_calls, 0);
}

TEST_F(component_observer_test, unmuting_does_not_replay_dispatches_dropped_while_muted) {
  int async_calls = 0;

  ecs_.component_observer<observed_value>().on_construct(
      [&](entt_ext::ecs&, entt_ext::entity, observed_value&) -> asio::awaitable<void> {
        ++async_calls;
        co_return;
      });

  ecs_.set_async_observers_muted(true);
  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 1);
  pump(ecs_);
  ASSERT_EQ(async_calls, 0);

  ecs_.set_async_observers_muted(false);
  pump(ecs_);

  // The mute is a drop, not a buffer. Any caller that needs the dropped
  // dispatches back has to record them itself and re-fire after unmuting.
  EXPECT_EQ(async_calls, 0);
}

TEST_F(component_observer_test, patch_after_unmute_refires_the_update_body_with_the_current_value) {
  std::vector<int> seen;

  ecs_.component_observer<observed_value>().on_update(
      [&](entt_ext::ecs&, entt_ext::entity, observed_value& value) -> asio::awaitable<void> {
        seen.push_back(value.value);
        co_return;
      });

  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 1);
  pump(ecs_);

  ecs_.set_async_observers_muted(true);
  ecs_.patch<observed_value>(entity, [](observed_value& value) {
    value.value = 42;
  });
  pump(ecs_);
  ASSERT_TRUE(seen.empty());

  ecs_.set_async_observers_muted(false);
  // Zero-mutation patch: the shape sync_client's drain_muted_update_replay()
  // uses to deliver an update whose dispatch was dropped mid-ingest.
  ecs_.patch<observed_value>(entity);
  pump(ecs_);

  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen.front(), 42) << "the re-fired body must see the value written while muted";
}

TEST_F(component_observer_test, async_body_sees_the_component_as_of_dispatch) {
  int observed = -1;

  ecs_.component_observer<observed_value>().on_update(
      [&](entt_ext::ecs&, entt_ext::entity, observed_value& value) -> asio::awaitable<void> {
        observed = value.value;
        co_return;
      });

  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 1);
  pump(ecs_);

  ecs_.patch<observed_value>(entity, [](observed_value& value) {
    value.value = 7;
  });

  // Deliberately mutating in place rather than through patch(): that is the
  // one thing a caller must never do to a synced component, and it is also
  // the only way to prove the queued body reads a snapshot instead of
  // re-fetching from storage when the channel drains.
  ecs_.get<observed_value>(entity).value = 99;
  pump(ecs_);

  EXPECT_EQ(observed, 7);
}

TEST_F(component_observer_test, destroy_body_receives_a_snapshot_of_the_removed_component) {
  int calls    = 0;
  int observed = -1;

  ecs_.component_observer<observed_value>().on_destroy(
      [&](entt_ext::ecs&, entt_ext::entity, observed_value& value) -> asio::awaitable<void> {
        ++calls;
        observed = value.value;
        co_return;
      });

  auto entity = ecs_.create();
  ecs_.emplace<observed_value>(entity, 5);
  pump(ecs_);

  ecs_.destroy(entity);
  ASSERT_FALSE(ecs_.valid(entity));
  pump(ecs_);

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(observed, 5) << "the body runs after the component is gone; it must read the dispatch-time snapshot";
}

} // namespace
