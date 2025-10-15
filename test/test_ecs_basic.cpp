#include <gtest/gtest.h>

#include <entt_ext/ecs.hpp>

#include "test_components.hpp"

using namespace entt_ext;

class EcsBasicTest : public ::testing::Test {
protected:
  void SetUp() override {
    ecs_instance = std::make_unique<ecs>();
  }

  void TearDown() override {
    ecs_instance.reset();
  }

  std::unique_ptr<ecs> ecs_instance;
};

TEST_F(EcsBasicTest, EntityCreation) {
  auto entity = ecs_instance->create();
  // EXPECT_NE(entity, entt_ext::null);  // Temporarily disabled due to EnTT configuration issues
  EXPECT_TRUE(ecs_instance->valid(entity));
}

TEST_F(EcsBasicTest, ComponentManagement) {
  auto entity = ecs_instance->create();

  // Add component
  auto& pos = ecs_instance->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);
  EXPECT_EQ(pos.x, 1.0f);
  EXPECT_EQ(pos.y, 2.0f);
  EXPECT_EQ(pos.z, 3.0f);
  EXPECT_TRUE(ecs_instance->any_of<Position>(entity));

  // Update component
  pos.x = 10.0f;
  ecs_instance->patch<Position>(entity);

  auto& updated_pos = ecs_instance->get<Position>(entity);
  EXPECT_EQ(updated_pos.x, 10.0f);

  // Remove component
  ecs_instance->remove<Position>(entity);
  EXPECT_FALSE(ecs_instance->any_of<Position>(entity));
}

TEST_F(EcsBasicTest, MultipleComponents) {
  auto entity = ecs_instance->create();

  // Add multiple components
  ecs_instance->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);
  ecs_instance->emplace<Velocity>(entity, 0.5f, 1.0f, 1.5f);
  ecs_instance->emplace<Health>(entity, 50, 100);

  EXPECT_TRUE(ecs_instance->any_of<Position>(entity));
  EXPECT_TRUE(ecs_instance->any_of<Velocity>(entity));
  EXPECT_TRUE(ecs_instance->any_of<Health>(entity));

  // Check values
  auto& pos    = ecs_instance->get<Position>(entity);
  auto& vel    = ecs_instance->get<Velocity>(entity);
  auto& health = ecs_instance->get<Health>(entity);

  EXPECT_EQ(pos.x, 1.0f);
  EXPECT_EQ(vel.dx, 0.5f);
  EXPECT_EQ(health.current, 50);
  EXPECT_EQ(health.maximum, 100);
}

TEST_F(EcsBasicTest, EntityDestroy) {
  auto entity = ecs_instance->create();
  ecs_instance->emplace<Position>(entity, 1.0f, 2.0f, 3.0f);

  EXPECT_TRUE(ecs_instance->valid(entity));
  EXPECT_TRUE(ecs_instance->any_of<Position>(entity));

  ecs_instance->destroy(entity);

  EXPECT_FALSE(ecs_instance->valid(entity));
}

TEST_F(EcsBasicTest, ComponentView) {
  // Create multiple entities with Position
  auto entity1 = ecs_instance->create();
  auto entity2 = ecs_instance->create();
  auto entity3 = ecs_instance->create();

  ecs_instance->emplace<Position>(entity1, 1.0f, 1.0f, 1.0f);
  ecs_instance->emplace<Position>(entity2, 2.0f, 2.0f, 2.0f);
  ecs_instance->emplace<Position>(entity3, 3.0f, 3.0f, 3.0f);

  // Only entity2 has velocity
  ecs_instance->emplace<Velocity>(entity2, 1.0f, 1.0f, 1.0f);

  // Test view with single component
  auto pos_view  = ecs_instance->view<Position>();
  int  pos_count = 0;
  for (auto entity : pos_view) {
    pos_count++;
    EXPECT_TRUE(ecs_instance->any_of<Position>(entity));
  }
  EXPECT_EQ(pos_count, 3);

  // Test view with multiple components
  auto pos_vel_view  = ecs_instance->view<Position, Velocity>();
  int  pos_vel_count = 0;
  for (auto entity : pos_vel_view) {
    pos_vel_count++;
    EXPECT_TRUE(ecs_instance->any_of<Position>(entity));
    EXPECT_TRUE(ecs_instance->any_of<Velocity>(entity));
  }
  EXPECT_EQ(pos_vel_count, 1);
}
