# entt_ext

An **[EnTT](https://github.com/skypjack/entt) extension** that uses **C++ coroutines** and **Boost.Asio** to provide an asynchronous, concurrent Entity-Component-System (ECS) framework with advanced features like systems, synchronization, and graph-based node processing.

Built on top of the excellent [EnTT](https://github.com/skypjack/entt) library by Michele Caini, this extension adds coroutine-based asynchronous capabilities and advanced system scheduling to the fast and reliable ECS foundation.

## Features

- 🚀 **Asynchronous ECS**: Built on C++20 coroutines and Boost.Asio for non-blocking operations
- ⚡ **Concurrent Systems**: Support for parallel, sequential, and detached system execution policies
- 🔄 **Entity Synchronization**: Client-server synchronization with serializable snapshots
- 🌐 **Network Ready**: Built-in RPC support for distributed ECS architectures
- 🎯 **Component Events**: Lifecycle hooks for construct, destroy, and update events
- 📊 **Graph Processing**: Node-based processing system (non-EMSCRIPTEN builds)
- 💾 **State Persistence**: Automatic serialization/deserialization with Cereal
- 🔧 **Double Buffering**: Thread-safe double buffer implementation for concurrent access
- 🏗️ **Module System**: Structured module imports and dependency management

## Requirements

- **C++23** compatible compiler
- **Boost** (Asio, coroutines)
- **EnTT** (Entity-Component-System library)
- **Cereal** (serialization library)
- **Meson** build system

## Basic Usage

### 1. Creating an ECS Instance

```cpp
#include <entt_ext/ecs.hpp>

int main() {
    // Create ECS instance with optional persistence file
    entt_ext::ecs ecs("my_app_state.bin");

    // Run the ECS main loop
    ecs.run();

    return 0;
}
```

### 2. Defining Components

```cpp
// Simple component
struct position {
    float x, y, z;

    // Cereal serialization support
    template<typename Archive>
    void serialize(Archive& ar) {
        ar(x, y, z);
    }
};

struct velocity {
    float dx, dy, dz;

    template<typename Archive>
    void serialize(Archive& ar) {
        ar(dx, dy, dz);
    }
};

// Tag component (no data)
struct movable {};
```

### 3. Creating Entities and Components

```cpp
void setup_entities(entt_ext::ecs& ecs) {
    // Create entities
    auto entity1 = ecs.create();
    auto entity2 = ecs.create();

    // Add components
    ecs.emplace<position>(entity1, 0.0f, 0.0f, 0.0f);
    ecs.emplace<velocity>(entity1, 1.0f, 0.0f, 0.0f);
    ecs.emplace<movable>(entity1);

    ecs.emplace<position>(entity2, 10.0f, 5.0f, 0.0f);
    // entity2 has no velocity - it won't move
}
```

### 4. Creating Systems

Systems in `entt_ext` are powerful and support different execution policies:

#### Sequential System (default)

```cpp
void create_movement_system(entt_ext::ecs& ecs) {
    ecs.system<position, velocity, movable>()
        .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity,
                 position& pos, velocity& vel, movable& tag) {
            // Update position based on velocity
            pos.x += vel.dx * dt;
            pos.y += vel.dy * dt;
            pos.z += vel.dz * dt;

            std::cout << "Entity " << static_cast<uint32_t>(entity)
                      << " moved to (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
        })
        .interval(1.0/60.0); // Run at 60 FPS
}
```

#### Parallel System (async)

```cpp
void create_async_system(entt_ext::ecs& ecs) {
    ecs.system<position>()
        .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity,
                 position& pos) -> entt_ext::asio::awaitable<void> {
            // Async operation
            co_await entt_ext::asio::post(ecs.concurrent_io_context(),
                                          entt_ext::asio::use_awaitable);

            // Some expensive computation
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::cout << "Async processing entity " << static_cast<uint32_t>(entity) << "\n";
            co_return;
        }, entt_ext::make_run_policy<entt_ext::run_policy::parallel>);
}
```

#### System with Custom Execution Policy

```cpp
void create_initialization_system(entt_ext::ecs& ecs) {
    ecs.system<position>()
        .each_once([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity,
                      position& pos) {
            std::cout << "Initializing entity " << static_cast<uint32_t>(entity) << "\n";
            // This runs only once per entity
        });
}
```

### 5. Component Lifecycle Events

```cpp
struct health {
    int current = 100;
    int maximum = 100;

    template<typename Archive>
    void serialize(Archive& ar) {
        ar(current, maximum);
    }
};

void setup_component_events(entt_ext::ecs& ecs) {
    // Register component and set up event handlers
    auto& health_component = ecs.component<health>();

    // Handle component construction
    health_component.on_construct([](entt_ext::ecs& ecs, entt_ext::entity entity, health& h) {
        std::cout << "Health component created for entity " << static_cast<uint32_t>(entity) << "\n";
        h.current = h.maximum; // Initialize to full health
    });

    // Handle component destruction
    health_component.on_destroy([](entt_ext::ecs& ecs, entt_ext::entity entity, health& h) {
        std::cout << "Entity " << static_cast<uint32_t>(entity) << " died with "
                  << h.current << " health remaining\n";
    });

    // Handle component updates
    health_component.on_update([](entt_ext::ecs& ecs, entt_ext::entity entity, health& h) {
        if (h.current <= 0) {
            std::cout << "Entity " << static_cast<uint32_t>(entity) << " has died!\n";
            ecs.delete_later(entity);
        }
    });
}
```

### 6. Module System

Create reusable modules to organize your code:

```cpp
namespace my_game {

struct game_module {
    game_module(entt_ext::ecs& ecs) {
        // Set up systems
        create_movement_system(ecs);
        create_combat_system(ecs);
        setup_component_events(ecs);

        std::cout << "Game module initialized\n";
    }

private:
    void create_combat_system(entt_ext::ecs& ecs) {
        ecs.system<health>()
            .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity, health& h) {
                // Regenerate health over time
                if (h.current < h.maximum) {
                    h.current = std::min(h.maximum, h.current + static_cast<int>(10 * dt));
                }
            })
            .interval(1.0); // Run every second
    }
};

void init_modules(entt_ext::ecs& ecs) {
    // Import modules
    entt_ext::import<game_module>(ecs);
}

} // namespace my_game
```

### 7. Complete Example

```cpp
#include <entt_ext/ecs.hpp>
#include <iostream>
#include <thread>

// Components
struct position {
    float x = 0, y = 0, z = 0;
    template<typename Archive> void serialize(Archive& ar) { ar(x, y, z); }
};

struct velocity {
    float dx = 0, dy = 0, dz = 0;
    template<typename Archive> void serialize(Archive& ar) { ar(dx, dy, dz); }
};

struct health {
    int current = 100, maximum = 100;
    template<typename Archive> void serialize(Archive& ar) { ar(current, maximum); }
};

// Tags
struct player {};
struct enemy {};

// Game module
struct game_module {
    game_module(entt_ext::ecs& ecs) {
        // Movement system
        ecs.system<position, velocity>()
            .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity,
                     position& pos, velocity& vel) {
                pos.x += vel.dx * dt;
                pos.y += vel.dy * dt;
                pos.z += vel.dz * dt;
            })
            .interval(1.0/60.0); // 60 FPS

        // Health regeneration system
        ecs.system<health>()
            .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity, health& h) {
                if (h.current < h.maximum) {
                    h.current = std::min(h.maximum, h.current + static_cast<int>(5 * dt));
                }
            })
            .interval(1.0); // Every second

        // Cleanup system for dead entities
        ecs.system<health>()
            .each([](entt_ext::ecs& ecs, double dt, entt_ext::entity entity, health& h) {
                if (h.current <= 0) {
                    std::cout << "Entity " << static_cast<uint32_t>(entity) << " died\n";
                    ecs.delete_later(entity);
                }
            });
    }
};

int main() {
    entt_ext::ecs ecs("game_state.bin");

    // Import game module
    entt_ext::import<game_module>(ecs);

    // Create player
    auto player_entity = ecs.create();
    ecs.emplace<position>(player_entity, 0.0f, 0.0f, 0.0f);
    ecs.emplace<velocity>(player_entity, 1.0f, 0.5f, 0.0f);
    ecs.emplace<health>(player_entity, 100, 100);
    ecs.emplace<player>(player_entity);

    // Create enemy
    auto enemy_entity = ecs.create();
    ecs.emplace<position>(enemy_entity, 10.0f, 10.0f, 0.0f);
    ecs.emplace<velocity>(enemy_entity, -0.5f, -0.3f, 0.0f);
    ecs.emplace<health>(enemy_entity, 50, 50);
    ecs.emplace<enemy>(enemy_entity);

    std::cout << "Starting game loop...\n";

    // Run the game
    ecs.run();

    return 0;
}
```

## Advanced Features

### Entity Synchronization

`entt_ext` provides powerful client-server synchronization capabilities:

```cpp
#include <entt_ext/ecs_sync.hpp>

// Server setup
void setup_server(entt_ext::ecs& server_ecs) {
    using SyncComponents = std::tuple<position, velocity, health>;

    auto sync_server = entt_ext::sync::sync_server<position, velocity, health>(server_ecs);

    // Enable sync for entities
    auto entity = server_ecs.create();
    server_ecs.emplace<position>(entity, 0, 0, 0);
    sync_server.enable_sync(entity);
}

// Client setup
void setup_client(entt_ext::ecs& client_ecs) {
    auto sync_client = entt_ext::sync::sync_client<position, velocity, health>(client_ecs);

    // Apply server state
    // sync_response response = get_from_server();
    // co_await sync_client.apply_sync_response(response);
}
```

### Double Buffering

For thread-safe concurrent access:

```cpp
entt_ext::double_buffer<int> thread_safe_counter;

// Writer thread
thread_safe_counter.write() = 42;
thread_safe_counter.swap();

// Reader thread
int value = thread_safe_counter.read(); // Always safe to read
```

### Child-Parent Relationships

```cpp
struct transform {};

void setup_hierarchy(entt_ext::ecs& ecs) {
    auto parent = ecs.create();
    auto child1 = ecs.create();
    auto child2 = ecs.create();

    // Create parent-child relationships
    ecs.emplace_child<transform>(parent, child1);
    ecs.emplace_child<transform>(parent, child2);

    // Iterate over children
    ecs.each_child<transform>(parent, [](entt_ext::ecs& ecs, entt_ext::entity parent,
                                         entt_ext::entity child, transform& t) {
        std::cout << "Processing child " << static_cast<uint32_t>(child)
                  << " of parent " << static_cast<uint32_t>(parent) << "\n";
    });
}
```

## Build Integration

### Meson Subproject

Add to your `meson.build`:

```meson
entt_ext_proj = subproject('entt_ext')
entt_ext_dep = entt_ext_proj.get_variable('entt_ext_dep')

executable('my_app',
  sources: ['main.cpp'],
  dependencies: [entt_ext_dep]
)
```

### CMake Integration

If using CMake, you can wrap the Meson build or use it as an external project.

## Performance Considerations

- **System Intervals**: Use appropriate intervals for systems to balance performance and responsiveness
- **Parallel Systems**: Use parallel execution for CPU-intensive operations
- **Component Size**: Keep components small and focused for better cache performance
- **Entity Cleanup**: Use `delete_later()` for safe entity destruction during system execution

## Thread Safety

- **Main Thread**: Entity creation, component management, and system scheduling
- **Concurrent Thread Pool**: Parallel system execution and async operations
- **Double Buffers**: Thread-safe data sharing between threads
- **Deferred Operations**: Safe component/entity modifications during parallel execution

## License

MIT License - see LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Add tests for new functionality
4. Submit a pull request

## See Also

- [EnTT Repository](https://github.com/skypjack/entt) - The original fast and reliable ECS library
- [EnTT Documentation](https://github.com/skypjack/entt/wiki) - Official EnTT documentation and wiki
- [Boost.Asio Documentation](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
- [Cereal Serialization](https://uscilab.github.io/cereal/)
