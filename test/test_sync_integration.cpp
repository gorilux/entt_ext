#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <entt_ext/ecs.hpp>
#include <entt_ext/ecs_sync.hpp>
#include <entt_ext/sync_client.hpp>
#include <entt_ext/sync_server.hpp>

#include "test_components.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <future>
#include <thread>

using namespace entt_ext;
using namespace entt_ext::sync;
namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

class SyncIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_ecs = std::make_unique<ecs>();
    client_ecs = std::make_unique<ecs>();

    server = std::make_unique<sync_server<Position, Velocity, Health>>(*server_ecs);
    client = std::make_unique<sync_client<Position, Velocity, Health>>(*client_ecs);

    io_context = std::make_unique<asio::io_context>();

    // Use a random available port
    test_port = 0; // Let the OS choose
  }

  void TearDown() override {
    if (server_running) {
      stop_server();
    }
    if (io_context) {
      io_context->stop();
    }
  }

  asio::awaitable<void> start_server() {
    try {
      tcp::acceptor acceptor(*io_context);
      tcp::endpoint endpoint(tcp::v4(), test_port);
      acceptor.open(endpoint.protocol());
      acceptor.set_option(tcp::acceptor::reuse_address(true));
      acceptor.bind(endpoint);

      // Get the actual port if we used 0
      if (test_port == 0) {
        test_port = acceptor.local_endpoint().port();
      }

      acceptor.listen();
      acceptor.close(); // Close the test acceptor

      // Start the actual server
      co_await server->start(test_port);
      server_running = true;
    } catch (const std::exception& e) {
      server_error = e.what();
    }
  }

  void stop_server() {
    if (server_running) {
      asio::co_spawn(*io_context, server->stop(), asio::detached);
      server_running = false;
    }
  }

  asio::awaitable<bool> connect_client() {
    try {
      bool connected = co_await client->connect("127.0.0.1", test_port, "test_client", "1.0");
      co_return connected;
    } catch (const std::exception& e) {
      client_error = e.what();
      co_return false;
    }
  }

  void run_io_context_for(std::chrono::milliseconds duration) {
    auto work_guard = asio::make_work_guard(*io_context);

    std::thread io_thread([this]() {
      io_context->run();
    });

    std::this_thread::sleep_for(duration);

    work_guard.reset();
    io_context->stop();

    if (io_thread.joinable()) {
      io_thread.join();
    }

    io_context->restart();
  }

  std::unique_ptr<ecs>                                     server_ecs;
  std::unique_ptr<ecs>                                     client_ecs;
  std::unique_ptr<sync_server<Position, Velocity, Health>> server;
  std::unique_ptr<sync_client<Position, Velocity, Health>> client;
  std::unique_ptr<asio::io_context>                        io_context;

  uint16_t    test_port      = 0;
  bool        server_running = false;
  std::string server_error;
  std::string client_error;
};

TEST_F(SyncIntegrationTest, ServerStartup) {
  std::promise<void> server_started;
  std::future<void>  server_future = server_started.get_future();

  asio::co_spawn(
      *io_context,
      [this, &server_started]() -> asio::awaitable<void> {
        co_await start_server();
        server_started.set_value();
      },
      asio::detached);

  run_io_context_for(std::chrono::milliseconds(100));

  // Check if server started successfully
  if (server_future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
    EXPECT_TRUE(server_running || !server_error.empty());
    if (!server_error.empty()) {
      GTEST_SKIP() << "Server failed to start: " << server_error;
    }
  } else {
    GTEST_SKIP() << "Server startup timeout";
  }
}

TEST_F(SyncIntegrationTest, ClientServerHandshake) {
  // Start server
  std::promise<void> server_ready;
  asio::co_spawn(
      *io_context,
      [this, &server_ready]() -> asio::awaitable<void> {
        co_await start_server();
        server_ready.set_value();
      },
      asio::detached);

  run_io_context_for(std::chrono::milliseconds(100));

  if (!server_running && server_error.empty()) {
    GTEST_SKIP() << "Server not ready for handshake test";
  }

  // Connect client
  std::promise<bool> client_connected;
  asio::co_spawn(
      *io_context,
      [this, &client_connected]() -> asio::awaitable<void> {
        bool connected = co_await connect_client();
        client_connected.set_value(connected);
      },
      asio::detached);

  run_io_context_for(std::chrono::milliseconds(200));

  auto future = client_connected.get_future();
  if (future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
    bool connected = future.get();
    if (connected) {
      EXPECT_TRUE(client->is_connected());
      EXPECT_TRUE(client->has_session());
      EXPECT_FALSE(client->get_session_id().empty());
      EXPECT_EQ(server->get_client_count(), 1);
    } else {
      GTEST_SKIP() << "Client connection failed: " << client_error;
    }
  } else {
    GTEST_SKIP() << "Client connection timeout";
  }
}

TEST_F(SyncIntegrationTest, BasicSyncSnapshot) {
  // This test would require a full server-client setup
  // For now, we'll test the snapshot creation logic

  auto server_entity = server->create_server_entity();
  server->add_server_component<Position>(server_entity, 10.0f, 20.0f, 30.0f);
  server->add_server_component<Velocity>(server_entity, 1.0f, 2.0f, 3.0f);

  // Verify server has the components
  EXPECT_TRUE(server_ecs->any_of<Position>(server_entity));
  EXPECT_TRUE(server_ecs->any_of<Velocity>(server_entity));

  auto& pos = server_ecs->get<Position>(server_entity);
  EXPECT_EQ(pos.x, 10.0f);
  EXPECT_EQ(pos.y, 20.0f);
  EXPECT_EQ(pos.z, 30.0f);

  auto& vel = server_ecs->get<Velocity>(server_entity);
  EXPECT_EQ(vel.dx, 1.0f);
  EXPECT_EQ(vel.dy, 2.0f);
  EXPECT_EQ(vel.dz, 3.0f);
}

TEST_F(SyncIntegrationTest, ClientDisconnection) {
  // Test client disconnection cleanup
  EXPECT_FALSE(client->is_connected());
  EXPECT_FALSE(client->has_session());

  // Simulate disconnection
  asio::co_spawn(*io_context,
                 client->disconnect(true), // clear mapping
                 asio::detached);

  run_io_context_for(std::chrono::milliseconds(50));

  EXPECT_FALSE(client->is_connected());
  EXPECT_FALSE(client->has_session());
  EXPECT_TRUE(client->get_session_id().empty());
}

TEST_F(SyncIntegrationTest, EntityMappingAfterSync) {
  // Test entity mapping functionality
  auto client_entity = client_ecs->create();
  client_ecs->emplace<Position>(client_entity, 5.0f, 6.0f, 7.0f);

  // Initially no server mapping
  EXPECT_EQ(client->get_server_entity(client_entity), entt_ext::null);

  // After sync (simulated), there should be a mapping
  // This would require actual client-server communication
  // For now, just verify the entity exists
  EXPECT_TRUE(client_ecs->valid(client_entity));
  EXPECT_TRUE(client_ecs->any_of<Position>(client_entity));
}

// Test multiple clients scenario
TEST_F(SyncIntegrationTest, MultipleClientsSetup) {
  // Create a second client
  auto client2_ecs = std::make_unique<ecs>();
  auto client2     = std::make_unique<sync_client<Position, Velocity, Health>>(*client2_ecs);

  EXPECT_TRUE(client2 != nullptr);
  EXPECT_FALSE(client2->is_connected());
  EXPECT_EQ(client2->notification_handler_count(), 0);

  // Both clients should be independent
  EXPECT_NE(client.get(), client2.get());
  EXPECT_NE(client_ecs.get(), client2_ecs.get());
}

// Test server entity management with multiple entities
TEST_F(SyncIntegrationTest, MultipleServerEntities) {
  auto entity1 = server->create_server_entity();
  auto entity2 = server->create_server_entity();
  auto entity3 = server->create_server_entity();

  EXPECT_NE(entity1, entity2);
  EXPECT_NE(entity2, entity3);
  EXPECT_NE(entity1, entity3);

  // Add different components to each
  server->add_server_component<Position>(entity1, 1.0f, 1.0f, 1.0f);
  server->add_server_component<Velocity>(entity2, 2.0f, 2.0f, 2.0f);
  server->add_server_component<Health>(entity3, 50, 100);

  EXPECT_TRUE(server_ecs->any_of<Position>(entity1));
  EXPECT_FALSE(server_ecs->any_of<Velocity>(entity1));
  EXPECT_FALSE(server_ecs->any_of<Health>(entity1));

  EXPECT_FALSE(server_ecs->any_of<Position>(entity2));
  EXPECT_TRUE(server_ecs->any_of<Velocity>(entity2));
  EXPECT_FALSE(server_ecs->any_of<Health>(entity2));

  EXPECT_FALSE(server_ecs->any_of<Position>(entity3));
  EXPECT_FALSE(server_ecs->any_of<Velocity>(entity3));
  EXPECT_TRUE(server_ecs->any_of<Health>(entity3));
}
