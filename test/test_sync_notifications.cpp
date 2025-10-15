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
#include <string>
#include <thread>
#include <vector>

using namespace entt_ext;
using namespace entt_ext::sync;

class SyncNotificationTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_ecs = std::make_unique<ecs>();
    client_ecs = std::make_unique<ecs>();

    server = std::make_unique<sync_server<Position, Velocity, Health, Name>>(*server_ecs);
    client = std::make_unique<sync_client<Position, Velocity, Health, Name>>(*client_ecs);

    // Reset test state
    position_updates.clear();
    velocity_updates.clear();
    health_updates.clear();
    name_updates.clear();
    removals.clear();
    notification_count = 0;
  }

  std::unique_ptr<ecs>                                           server_ecs;
  std::unique_ptr<ecs>                                           client_ecs;
  std::unique_ptr<sync_server<Position, Velocity, Health, Name>> server;
  std::unique_ptr<sync_client<Position, Velocity, Health, Name>> client;

  // Test data collection
  std::vector<Position>    position_updates;
  std::vector<Velocity>    velocity_updates;
  std::vector<Health>      health_updates;
  std::vector<Name>        name_updates;
  std::vector<std::string> removals;
  std::atomic<int>         notification_count{0};
};

TEST_F(SyncNotificationTest, NotificationHandlerRegistration) {
  // Test basic registration
  client->register_notification_handler<Position>("pos_update", [this](Position pos) {
    position_updates.push_back(pos);
    notification_count++;
  });

  EXPECT_EQ(client->notification_handler_count(), 1);
  EXPECT_TRUE(client->has_notification_handler("pos_update"));

  // Test multiple handlers
  client->register_notification_handler<Velocity>("vel_update", [this](Velocity vel) {
    velocity_updates.push_back(vel);
    notification_count++;
  });

  client->register_notification_handler<Health>("health_update", [this](Health health) {
    health_updates.push_back(health);
    notification_count++;
  });

  EXPECT_EQ(client->notification_handler_count(), 3);
  EXPECT_TRUE(client->has_notification_handler("vel_update"));
  EXPECT_TRUE(client->has_notification_handler("health_update"));
}

TEST_F(SyncNotificationTest, NotificationHandlerUnregistration) {
  // Register multiple handlers
  client->register_notification_handler<Position>("pos1", [](Position pos) {});
  client->register_notification_handler<Position>("pos2", [](Position pos) {});
  client->register_notification_handler<Velocity>("vel1", [](Velocity vel) {});

  EXPECT_EQ(client->notification_handler_count(), 3);

  // Unregister one
  client->unregister_notification_handler("pos1");
  EXPECT_EQ(client->notification_handler_count(), 2);
  EXPECT_FALSE(client->has_notification_handler("pos1"));
  EXPECT_TRUE(client->has_notification_handler("pos2"));
  EXPECT_TRUE(client->has_notification_handler("vel1"));

  // Unregister non-existent (should not crash)
  client->unregister_notification_handler("nonexistent");
  EXPECT_EQ(client->notification_handler_count(), 2);

  // Clear all
  client->clear_notification_handlers();
  EXPECT_EQ(client->notification_handler_count(), 0);
  EXPECT_FALSE(client->has_notification_handler("pos2"));
  EXPECT_FALSE(client->has_notification_handler("vel1"));
}

TEST_F(SyncNotificationTest, LambdaHandlerRegistration) {
  // Test lambda with capture
  std::vector<Position> captured_positions;

  client->register_notification_handler<Position>("lambda_test", [&captured_positions](Position pos) {
    captured_positions.push_back(pos);
  });

  EXPECT_EQ(client->notification_handler_count(), 1);
  EXPECT_TRUE(client->has_notification_handler("lambda_test"));

  // Test different lambda signatures
  client->register_notification_handler<Velocity>("vel_lambda", [this](Velocity vel) {
    velocity_updates.push_back(vel);
  });

  client->register_notification_handler("no_params_lambda", [this]() {
    notification_count++;
  });

  EXPECT_EQ(client->notification_handler_count(), 3);
}

TEST_F(SyncNotificationTest, MultipleParameterHandlers) {
  // Test handlers with different parameter counts
  client->register_notification_handler<Position>("single_param", [this](Position pos) {
    position_updates.push_back(pos);
  });

  client->register_notification_handler<Position, Velocity>("two_params", [this](Position pos, Velocity vel) {
    position_updates.push_back(pos);
    velocity_updates.push_back(vel);
  });

  client->register_notification_handler<Position, Velocity, Health>("three_params", [this](Position pos, Velocity vel, Health health) {
    position_updates.push_back(pos);
    velocity_updates.push_back(vel);
    health_updates.push_back(health);
  });

  EXPECT_EQ(client->notification_handler_count(), 3);
}

TEST_F(SyncNotificationTest, NotificationToggling) {
  bool handler_called = false;

  client->register_notification_handler<Position>("toggle_test", [&handler_called](Position pos) {
    handler_called = true;
  });

  // Initially enabled
  EXPECT_TRUE(client->are_notifications_enabled());

  // Disable notifications
  client->set_notifications_enabled(false);
  EXPECT_FALSE(client->are_notifications_enabled());

  // Re-enable
  client->set_notifications_enabled(true);
  EXPECT_TRUE(client->are_notifications_enabled());
}

TEST_F(SyncNotificationTest, ServerNotificationToggling) {
  // Test server notification control
  EXPECT_TRUE(server->are_notifications_enabled());

  server->set_notifications_enabled(false);
  EXPECT_FALSE(server->are_notifications_enabled());

  server->set_notifications_enabled(true);
  EXPECT_TRUE(server->are_notifications_enabled());
}

TEST_F(SyncNotificationTest, ComponentSpecificNotifications) {
  // Set up handlers for different component types
  client->register_notification_handler<Position>("position_changed", [this](Position pos) {
    position_updates.push_back(pos);
  });

  client->register_notification_handler<Velocity>("velocity_changed", [this](Velocity vel) {
    velocity_updates.push_back(vel);
  });

  client->register_notification_handler<Health>("health_changed", [this](Health health) {
    health_updates.push_back(health);
  });

  client->register_notification_handler<Name>("name_changed", [this](Name name) {
    name_updates.push_back(name);
  });

  EXPECT_EQ(client->notification_handler_count(), 4);

  // Verify each handler is registered correctly
  EXPECT_TRUE(client->has_notification_handler("position_changed"));
  EXPECT_TRUE(client->has_notification_handler("velocity_changed"));
  EXPECT_TRUE(client->has_notification_handler("health_changed"));
  EXPECT_TRUE(client->has_notification_handler("name_changed"));
}

TEST_F(SyncNotificationTest, RemovalNotifications) {
  // Set up removal handlers
  client->register_notification_handler("position_removed", [this]() {
    removals.push_back("position");
  });

  client->register_notification_handler("velocity_removed", [this]() {
    removals.push_back("velocity");
  });

  client->register_notification_handler("health_removed", [this]() {
    removals.push_back("health");
  });

  EXPECT_EQ(client->notification_handler_count(), 3);
  EXPECT_TRUE(client->has_notification_handler("position_removed"));
  EXPECT_TRUE(client->has_notification_handler("velocity_removed"));
  EXPECT_TRUE(client->has_notification_handler("health_removed"));
}

TEST_F(SyncNotificationTest, HandlerOverwriting) {
  int call_count_1 = 0;
  int call_count_2 = 0;

  // Register first handler
  client->register_notification_handler<Position>("overwrite_test", [&call_count_1](Position pos) {
    call_count_1++;
  });

  EXPECT_EQ(client->notification_handler_count(), 1);

  // Register second handler with same name (should overwrite)
  client->register_notification_handler<Position>("overwrite_test", [&call_count_2](Position pos) {
    call_count_2++;
  });

  // Should still have only 1 handler (overwritten)
  EXPECT_EQ(client->notification_handler_count(), 1);
  EXPECT_TRUE(client->has_notification_handler("overwrite_test"));
}

TEST_F(SyncNotificationTest, HandlerExceptionSafety) {
  bool good_handler_called = false;

  // Register handler that throws
  client->register_notification_handler<Position>("throwing_handler", [](Position pos) {
    throw std::runtime_error("Test exception");
  });

  // Register handler that should still work
  client->register_notification_handler<Position>("good_handler", [&good_handler_called](Position pos) {
    good_handler_called = true;
  });

  EXPECT_EQ(client->notification_handler_count(), 2);

  // Both handlers should be registered despite one throwing
  EXPECT_TRUE(client->has_notification_handler("throwing_handler"));
  EXPECT_TRUE(client->has_notification_handler("good_handler"));
}

TEST_F(SyncNotificationTest, ServerEntityNotificationSetup) {
  // Test that server properly sets up for notifications
  auto entity = server->create_server_entity();

  // Add components using server methods
  server->add_server_component<Position>(entity, 1.0f, 2.0f, 3.0f);
  server->add_server_component<Velocity>(entity, 0.5f, 1.0f, 1.5f);

  // Verify components were added
  EXPECT_TRUE(server_ecs->any_of<Position>(entity));
  EXPECT_TRUE(server_ecs->any_of<Velocity>(entity));

  auto& pos = server_ecs->get<Position>(entity);
  EXPECT_EQ(pos.x, 1.0f);
  EXPECT_EQ(pos.y, 2.0f);
  EXPECT_EQ(pos.z, 3.0f);

  auto& vel = server_ecs->get<Velocity>(entity);
  EXPECT_EQ(vel.dx, 0.5f);
  EXPECT_EQ(vel.dy, 1.0f);
  EXPECT_EQ(vel.dz, 1.5f);
}

TEST_F(SyncNotificationTest, ClientSilentApplicationTest) {
  auto entity = client_ecs->create();

  // Set up handler to verify it's not called during silent operations
  bool handler_called = false;
  client->register_notification_handler<Position>("silent_test", [&handler_called](Position pos) {
    handler_called = true;
  });

  // Silent application should not trigger handlers
  Position pos{5.0f, 6.0f, 7.0f};
  client->apply_component_update_silently<Position>(entity, std::move(pos));

  // Give time for any potential async operations
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Handler should not have been called
  EXPECT_FALSE(handler_called);

  // But component should be applied
  EXPECT_TRUE(client_ecs->any_of<Position>(entity));
  auto& applied_pos = client_ecs->get<Position>(entity);
  EXPECT_EQ(applied_pos.x, 5.0f);
  EXPECT_EQ(applied_pos.y, 6.0f);
  EXPECT_EQ(applied_pos.z, 7.0f);
}
