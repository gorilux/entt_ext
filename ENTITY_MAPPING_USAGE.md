# Entity Mapping System Usage

The entity mapping system solves the problem where client and server ECS instances have different entity IDs for the same logical entities. This system automatically maps between client and server entity IDs during synchronization.

## Key Components

### 1. Entity Mapping Class (`entity_mapping`)

- Provides bidirectional mapping between client and server entity IDs
- Automatically handles mapping creation, lookup, and cleanup
- Serializable for persistence

### 2. Entity Mapping Component (`entity_mapping_component`)

- ECS component that stores the entity mapping in the client registry
- Automatically initialized by `sync_client`

## Usage Examples

### Server Setup

```cpp
// Create server with Position and Velocity components to sync
entt_ext::sync::sync_server<Position, Velocity> server(ecs);

// Start server on port 8080
co_await server.start(8080);

// Entities are automatically synced when they have Position or Velocity components
// No need to explicitly enable sync
```

### Client Setup

```cpp
// Create client with same component types
entt_ext::sync::sync_client<Position, Velocity> client(ecs);

// Connect to server
bool connected = co_await client.connect("localhost", 8080, "MyClient", "1.0");

// Add sync components to entities - they will be automatically synced
entity player = ecs.create();
ecs.emplace<Position>(player, 10.0f, 20.0f);  // Automatically marked for sync
ecs.emplace<Velocity>(player, 1.0f, 0.5f);    // Automatically marked for sync

// The client entity will be automatically mapped to a server entity
// when components are first synchronized
```

### Entity Mapping Inspection

```cpp
// On client side - check if entity is mapped
bool is_mapped = client.is_entity_mapped(client_entity);

// Get corresponding server entity ID
entity server_entity = client.get_server_entity(client_entity);

// Get the full mapping for debugging
const auto& mapping = client.get_entity_mapping();
std::cout << "Mapped entities: " << mapping.size() << std::endl;
```

### Server-side Entity Management

```cpp
// Get mapping for a specific client (debugging)
const auto* mapping = server.get_client_mapping("session_id");

// Remove a client and cleanup their entities
server.remove_client("session_id");
```

## How It Works

### 1. Handshake and Session Creation

- Client connects and performs handshake
- Server creates unique session ID and client state
- Each client gets its own entity mapping

### 2. Entity Creation and Mapping

- When client creates/modifies a synced entity, it sends the client entity ID
- Server creates a new server entity using `ecs_.create()` (ECS manages entity IDs)
- Server maintains bidirectional mapping: client_entity ↔ server_entity
- Response includes both client and server entity IDs for mapping updates

### 3. Synchronization Process

- Client requests sync with client entity IDs
- Server maps to server entity IDs, creates snapshot
- Server sends snapshot with client entity IDs and mapping information
- Client applies snapshot and updates its entity mapping

### 4. Component Operations

- Component insert/update/remove operations use entity mapping for ownership
- Client sends client entity ID, server maps to server entity ID
- If client has entity mapping, they can modify that entity
- Insert/update operations use unified "update_component_X" endpoints
- Operations are performed on server entity, results sent back with mapping info

## Key Benefits

1. **Automatic Sync**: Entities with sync components are automatically synchronized
2. **Automatic Mapping**: No manual entity ID management required
3. **Session Isolation**: Each client has independent entity mappings
4. **Component-Driven**: Only entities with specified sync components are synchronized
5. **Persistence**: Mappings can be serialized and restored
6. **Debugging Support**: Full mapping inspection capabilities
7. **Cleanup**: Automatic cleanup when clients disconnect

## Thread Safety

- Server maintains per-client mappings, avoiding cross-client conflicts
- ECS handles entity ID generation and management internally
- All mapping operations are designed to be safe within async contexts

## Error Handling

- Invalid entity mappings return appropriate error messages
- Missing mappings are handled gracefully with clear error reporting
- Disconnected clients can optionally clear their mappings on reconnect
