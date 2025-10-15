#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <entt_ext/ecs.hpp>
#include <entt_ext/ecs_sync.hpp>
#include <entt_ext/sync_client.hpp>
#include <entt_ext/sync_server.hpp>

#include "test_components.hpp"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <thread>

using namespace entt_ext;
using namespace entt_ext::sync;

class SyncLoopPreventionTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_ecs = std::make_unique<ecs>();
    client_ecs = std::make_unique<ecs>();

    server = std::make_unique<sync_server<Position, Velocity, Health>>(*server_ecs);
    client = std::make_unique<sync_client<Position, Velocity, Health>>(*client_ecs);

    // Reset counters
    observer_trigger_count = 0;
    notification_count     = 0;
  }

  std::unique_ptr<ecs>                                     server_ecs;
  std::unique_ptr<ecs>                                     client_ecs;
  std::unique_ptr<sync_server<Position, Velocity, Health>> server;
  std::unique_ptr<sync_client<Position, Velocity, Health>> client;

  std::atomic<int> observer_trigger_count{0};
  std::atomic<int> notification_count{0};
};

TEST_F(SyncLoopPreventionTest, ClientSilentUpdate) {
  auto entity = client_ecs->create();

  // Set up observer to count triggers
  auto& observer = client_ecs->component_observer<Position>();
  observer.on_construct([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  observer.on_update([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  // Normal update should trigger observer
  client_ecs->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);

  // Give observer time to trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int normal_count = observer_trigger_count.load();
  EXPECT_GT(normal_count, 0); // Should have triggered

  // Silent update should NOT trigger observer
  Position silent_pos{4.0f, 5.0f, 6.0f};
  client->apply_component_update_silently<Position>(entity, std::move(silent_pos));

  // Give observer time to potentially trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int after_silent_count = observer_trigger_count.load();
  EXPECT_EQ(after_silent_count, normal_count); // Should NOT have increased

  // Verify the component was actually updated
  EXPECT_TRUE(client_ecs->any_of<Position>(entity));
  auto& pos = client_ecs->get<Position>(entity);
  EXPECT_EQ(pos.x, 4.0f);
  EXPECT_EQ(pos.y, 5.0f);
  EXPECT_EQ(pos.z, 6.0f);
}

TEST_F(SyncLoopPreventionTest, ClientSilentRemoval) {
  auto entity = client_ecs->create();
  client_ecs->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);

  // Set up observer to count triggers
  auto& observer = client_ecs->component_observer<Position>();
  observer.on_destroy([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  // Normal removal should trigger observer
  client_ecs->remove<Position>(entity);
  client_ecs->emplace<Position>(entity, 2.0f, 3.0f, 4.0f); // Re-add for silent test

  // Give observer time to trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int normal_count = observer_trigger_count.load();
  EXPECT_GT(normal_count, 0); // Should have triggered

  // Silent removal should NOT trigger observer
  client->apply_component_removal_silently<Position>(entity);

  // Give observer time to potentially trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int after_silent_count = observer_trigger_count.load();
  EXPECT_EQ(after_silent_count, normal_count); // Should NOT have increased

  // Verify the component was actually removed
  EXPECT_FALSE(client_ecs->any_of<Position>(entity));
}

TEST_F(SyncLoopPreventionTest, ServerSilentUpdate) {
  auto entity = server_ecs->create();

  // Set up observer to count triggers
  auto& observer = server_ecs->component_observer<Position>();
  observer.on_construct([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  observer.on_update([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  // Normal update should trigger observer
  server_ecs->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);

  // Give observer time to trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int normal_count = observer_trigger_count.load();
  EXPECT_GT(normal_count, 0); // Should have triggered

  // Silent update should NOT trigger observer
  Position silent_pos{4.0f, 5.0f, 6.0f};
  server->apply_component_update_silently<Position>(entity, silent_pos);

  // Give observer time to potentially trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int after_silent_count = observer_trigger_count.load();
  EXPECT_EQ(after_silent_count, normal_count); // Should NOT have increased

  // Verify the component was actually updated
  EXPECT_TRUE(server_ecs->any_of<Position>(entity));
  auto& pos = server_ecs->get<Position>(entity);
  EXPECT_EQ(pos.x, 4.0f);
  EXPECT_EQ(pos.y, 5.0f);
  EXPECT_EQ(pos.z, 6.0f);
}

TEST_F(SyncLoopPreventionTest, ServerSilentRemoval) {
  auto entity = server_ecs->create();
  server_ecs->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);

  // Set up observer to count triggers
  auto& observer = server_ecs->component_observer<Position>();
  observer.on_destroy([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  // Normal removal should trigger observer
  server_ecs->remove<Position>(entity);
  server_ecs->emplace<Position>(entity, 2.0f, 3.0f, 4.0f); // Re-add for silent test

  // Give observer time to trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int normal_count = observer_trigger_count.load();
  EXPECT_GT(normal_count, 0); // Should have triggered

  // Silent removal should NOT trigger observer
  size_t removed = server->apply_component_removal_silently<Position>(entity);
  EXPECT_EQ(removed, 1);

  // Give observer time to potentially trigger
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  int after_silent_count = observer_trigger_count.load();
  EXPECT_EQ(after_silent_count, normal_count); // Should NOT have increased

  // Verify the component was actually removed
  EXPECT_FALSE(server_ecs->any_of<Position>(entity));
}

TEST_F(SyncLoopPreventionTest, ClientApplyingRemoteChangesFlag) {
  auto entity = client_ecs->create();

  // Test the applying_remote_changes_ flag directly through silent operations
  Position pos1{1.0f, 2.0f, 3.0f};
  client->apply_component_update_silently<Position>(entity, std::move(pos1));

  EXPECT_TRUE(client_ecs->any_of<Position>(entity));

  // Multiple silent operations should work
  Position pos2{4.0f, 5.0f, 6.0f};
  client->apply_component_update_silently<Position>(entity, std::move(pos2));

  auto& final_pos = client_ecs->get<Position>(entity);
  EXPECT_EQ(final_pos.x, 4.0f);
  EXPECT_EQ(final_pos.y, 5.0f);
  EXPECT_EQ(final_pos.z, 6.0f);

  // Silent removal
  client->apply_component_removal_silently<Position>(entity);
  EXPECT_FALSE(client_ecs->any_of<Position>(entity));
}

TEST_F(SyncLoopPreventionTest, ServerApplyingClientChangesFlag) {
  auto entity = server_ecs->create();

  // Test the applying_client_changes_ flag directly through silent operations
  Position pos1{1.0f, 2.0f, 3.0f};
  server->apply_component_update_silently<Position>(entity, pos1);

  EXPECT_TRUE(server_ecs->any_of<Position>(entity));

  // Multiple silent operations should work
  Position pos2{4.0f, 5.0f, 6.0f};
  server->apply_component_update_silently<Position>(entity, pos2);

  auto& final_pos = server_ecs->get<Position>(entity);
  EXPECT_EQ(final_pos.x, 4.0f);
  EXPECT_EQ(final_pos.y, 5.0f);
  EXPECT_EQ(final_pos.z, 6.0f);

  // Silent removal
  size_t removed = server->apply_component_removal_silently<Position>(entity);
  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(server_ecs->any_of<Position>(entity));
}

TEST_F(SyncLoopPreventionTest, MixedSilentAndNormalOperations) {
  auto entity = client_ecs->create();

  // Set up observer to count triggers
  auto& observer = client_ecs->component_observer<Position>();
  observer.on_construct([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  observer.on_update([this](ecs& ecs, entt_ext::entity e, Position& component) -> boost::asio::awaitable<void> {
    observer_trigger_count++;
    co_return;
  });

  // Silent operation - should not trigger
  Position pos1{1.0f, 2.0f, 3.0f};
  client->apply_component_update_silently<Position>(entity, std::move(pos1));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  int count_after_silent = observer_trigger_count.load();
  EXPECT_EQ(count_after_silent, 0);

  // Normal operation - should trigger
  auto& pos = client_ecs->get<Position>(entity);
  pos.x     = 10.0f;
  client_ecs->patch<Position>(entity);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  int count_after_normal = observer_trigger_count.load();
  EXPECT_GT(count_after_normal, count_after_silent);

  // Another silent operation - should not trigger
  Position pos2{7.0f, 8.0f, 9.0f};
  client->apply_component_update_silently<Position>(entity, std::move(pos2));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  int count_after_second_silent = observer_trigger_count.load();
  EXPECT_EQ(count_after_second_silent, count_after_normal);

  // Verify final state
  auto& final_pos = client_ecs->get<Position>(entity);
  EXPECT_EQ(final_pos.x, 7.0f);
  EXPECT_EQ(final_pos.y, 8.0f);
  EXPECT_EQ(final_pos.z, 9.0f);
}

// Test that notifications are properly filtered during remote changes
TEST_F(SyncLoopPreventionTest, NotificationFilteringDuringRemoteChanges) {
  // Set up notification handler
  bool notification_received = false;
  client->register_notification_handler<Position>("test_notification", [&notification_received](Position pos) {
    notification_received = true;
  });

  // Simulate receiving a notification (this would normally come from the server)
  // For testing, we'll directly test the flag behavior

  // When not applying remote changes, notifications should be processed
  client->set_notifications_enabled(true);
  EXPECT_TRUE(client->are_notifications_enabled());

  // When disabled, notifications should be ignored
  client->set_notifications_enabled(false);
  EXPECT_FALSE(client->are_notifications_enabled());

  client->set_notifications_enabled(true);
  EXPECT_TRUE(client->are_notifications_enabled());
}
