// Example demonstrating Flecs-style deferred operations in entt_ext
#include <entt_ext/ecs.hpp>
#include <iostream>

// Example components
struct Position {
  float x, y;
};

struct Health {
  int value;
};

struct Bullet {
  float speed;
};

struct Enemy {
  bool should_spawn_bullet = false;
};

struct DeathEffect {
  Position origin;
};

// Example 1: Automatic deferring in systems
void example_automatic_system_deferring(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 1: Automatic System Deferring ===\n";

  // Create some test entities
  auto e1 = ecs.create();
  ecs.emplace<Position>(e1, 10.0f, 20.0f);
  ecs.emplace<Health>(e1, 0); // Dead

  auto e2 = ecs.create();
  ecs.emplace<Position>(e2, 30.0f, 40.0f);
  ecs.emplace<Health>(e2, 100); // Alive

  // System that destroys dead entities
  // All structural changes are automatically deferred!
  ecs.system<Position, Health>()
      .each([](entt_ext::ecs& ecs, entt_ext::system& sys, double dt, entt_ext::entity e, Position& pos, Health& health) {
        if (health.value <= 0) {
          std::cout << "Entity " << static_cast<int>(e) << " died at (" << pos.x << ", " << pos.y << ")\n";

          // These operations are automatically deferred - safe during iteration!
          ecs.destroy(e); // Will be deferred automatically

          // Spawn death effect
          auto effect = ecs.create();
          ecs.emplace<DeathEffect>(effect, pos);
          std::cout << "Created death effect entity " << static_cast<int>(effect) << "\n";
        }
      })
      .stage(entt_ext::stage::update);

  std::cout << "Running system (operations will be deferred automatically)...\n";
}

// Example 2: Manual defer with RAII guard
void example_manual_defer_guard(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 2: Manual Defer with RAII Guard ===\n";

  {
    entt_ext::defer_guard guard(ecs);

    std::cout << "Creating 10 entities with deferred operations...\n";
    for (int i = 0; i < 10; ++i) {
      auto e = ecs.create_deferred();
      ecs.emplace_deferred<Position>(e, static_cast<float>(i), static_cast<float>(i * 2));
      ecs.emplace_deferred<Health>(e, 100 + i);
    }

    std::cout << "All operations queued. They will flush when guard goes out of scope.\n";
  } // <-- All deferred operations execute here

  std::cout << "Guard destroyed, operations flushed!\n";
}

// Example 3: Explicit defer begin/end
void example_explicit_defer(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 3: Explicit Defer Begin/End ===\n";

  ecs.defer_begin();

  std::cout << "Is deferred: " << (ecs.is_deferred() ? "yes" : "no") << "\n";

  for (int i = 0; i < 5; ++i) {
    auto e = ecs.create_deferred();
    ecs.emplace_deferred<Position>(e, 100.0f + i, 200.0f + i);
  }

  std::cout << "Queued 5 entity creations. Calling defer_end()...\n";

  ecs.defer_end(); // <-- All operations execute here

  std::cout << "Operations flushed!\n";
}

// Example 4: Nested defer contexts
void example_nested_defer(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 4: Nested Defer Contexts ===\n";

  ecs.defer_begin();
  std::cout << "Level 1: defer_begin()\n";

  auto e1 = ecs.create_deferred();
  std::cout << "  Created entity at level 1\n";

  ecs.defer_begin();
  std::cout << "  Level 2: defer_begin() [nested]\n";

  auto e2 = ecs.create_deferred();
  std::cout << "    Created entity at level 2\n";

  ecs.defer_end();
  std::cout << "  Level 2: defer_end() [no flush - still nested]\n";

  auto e3 = ecs.create_deferred();
  std::cout << "  Created entity at level 1\n";

  ecs.defer_end();
  std::cout << "Level 1: defer_end() [flush all!]\n";
}

// Example 5: Suspend/resume for immediate operations
void example_suspend_resume(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 5: Suspend/Resume for Immediate Operations ===\n";

  // Create a test entity
  auto test_entity = ecs.create();
  ecs.emplace<Position>(test_entity, 50.0f, 60.0f);

  ecs.defer_begin();
  std::cout << "Defer mode started\n";

  // Queue some deferred operations
  auto e1 = ecs.create_deferred();
  ecs.emplace_deferred<Health>(e1, 100);
  std::cout << "Queued entity creation (deferred)\n";

  {
    // Need to read something immediately
    entt_ext::defer_suspend_guard suspend(ecs);
    std::cout << "Defer suspended temporarily\n";

    // This executes immediately
    auto& pos = ecs.get<Position>(test_entity);
    std::cout << "Read position immediately: (" << pos.x << ", " << pos.y << ")\n";

  } // Defer resumes here

  std::cout << "Defer resumed\n";

  // Back to deferred operations
  auto e2 = ecs.create_deferred();
  ecs.emplace_deferred<Health>(e2, 200);
  std::cout << "Queued another entity creation (deferred)\n";

  ecs.defer_end();
  std::cout << "All deferred operations flushed\n";
}

// Example 6: Manual flush
void example_manual_flush(entt_ext::ecs& ecs) {
  std::cout << "\n=== Example 6: Manual Flush ===\n";

  ecs.defer_begin();

  for (int batch = 0; batch < 3; ++batch) {
    std::cout << "Batch " << batch << ": Creating 5 entities\n";

    for (int i = 0; i < 5; ++i) {
      auto e = ecs.create_deferred();
      ecs.emplace_deferred<Position>(e, static_cast<float>(batch * 10 + i), 0.0f);
    }

    // Manually flush after each batch
    std::cout << "  Manual flush...\n";
    ecs.flush_commands();
  }

  ecs.defer_end();
  std::cout << "Final defer_end() [no commands to flush]\n";
}

int main() {
  std::cout << "Flecs-Style Deferred Operations Examples\n";
  std::cout << "=========================================\n";

  entt_ext::ecs ecs;

  example_automatic_system_deferring(ecs);
  example_manual_defer_guard(ecs);
  example_explicit_defer(ecs);
  example_nested_defer(ecs);
  example_suspend_resume(ecs);
  example_manual_flush(ecs);

  std::cout << "\n=== All Examples Completed ===\n";

  return 0;
}
