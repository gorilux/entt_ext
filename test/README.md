# EnTT Extension Sync System Tests

This directory contains comprehensive unit tests for the `sync_server` and `sync_client` components of the entt_ext library.

## Test Structure

### Test Files

1. **`test_sync_basic.cpp`** - Basic functionality tests

   - Server and client creation
   - Component management
   - Notification handler registration
   - Entity mapping
   - Silent operations

2. **`test_sync_integration.cpp`** - Integration tests

   - Server startup and client connection
   - Handshake process
   - Multi-client scenarios
   - Entity synchronization
   - Disconnection handling

3. **`test_sync_loop_prevention.cpp`** - Loop prevention tests

   - Silent update operations
   - Silent removal operations
   - Flag-based loop prevention
   - Mixed silent/normal operations
   - Observer trigger counting

4. **`test_sync_notifications.cpp`** - Notification system tests
   - Handler registration/unregistration
   - Lambda handlers
   - Multi-parameter handlers
   - Notification toggling
   - Exception safety
   - Component-specific notifications

### Test Components

**`test_components.hpp`** defines test components used across all tests:

- `Position` - 3D position with x, y, z coordinates
- `Velocity` - 3D velocity with dx, dy, dz components
- `Health` - Health component with current/maximum values
- `Name` - String name component

All test components implement:

- Equality operators for testing
- Cereal serialization for sync
- Type name specializations

## Running Tests

### Prerequisites

- Meson build system
- GoogleTest (automatically fetched)
- Boost.Asio
- C++23 compiler

### Build and Run

```bash
# From the entt_ext directory
./run_tests.sh
```

Or manually:

```bash
# Setup build directory
meson setup build

# Compile tests
meson compile -C build

# Run tests
meson test -C build --verbose
```

### Individual Test Execution

```bash
# Run specific test suite
./build/test/entt_ext_tests --gtest_filter="SyncBasicTest.*"

# Run with verbose output
./build/test/entt_ext_tests --gtest_filter="*" --gtest_verbose
```

## Test Coverage

### Basic Functionality ✅

- [x] Server/client creation and initialization
- [x] Component management (add/update/remove)
- [x] Notification handler registration
- [x] Entity mapping functionality
- [x] Silent operations for loop prevention

### Integration Testing ✅

- [x] Server startup and binding
- [x] Client connection and handshake
- [x] Multi-client support
- [x] Disconnection cleanup
- [x] Entity synchronization basics

### Loop Prevention ✅

- [x] Silent component updates
- [x] Silent component removals
- [x] Observer trigger prevention
- [x] Flag-based loop detection
- [x] Mixed operation scenarios

### Notification System ✅

- [x] Handler registration/unregistration
- [x] Lambda function handlers
- [x] Multi-parameter notifications
- [x] Notification enable/disable
- [x] Exception safety in handlers
- [x] Component-specific notifications

### Advanced Scenarios 🔄

- [ ] Full client-server communication
- [ ] Real-time notification delivery
- [ ] Network error handling
- [ ] Performance under load
- [ ] Concurrent client operations

## Test Architecture

### Mock Components

Tests use lightweight mock components that implement the required interfaces without complex business logic.

### Async Testing

Integration tests use Boost.Asio's `io_context` with controlled execution to test asynchronous operations.

### Loop Prevention Verification

Loop prevention tests use atomic counters and observer callbacks to verify that silent operations don't trigger sync loops.

### Exception Safety

Tests verify that the sync system handles exceptions gracefully without corrupting state.

## Adding New Tests

### Test Naming Convention

- Test fixtures: `Sync[Feature]Test`
- Test cases: `[Feature][Scenario]`
- Example: `SyncBasicTest.ComponentManagement`

### Test Structure

```cpp
class SyncNewFeatureTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Initialize test components
  }

  void TearDown() override {
    // Cleanup
  }

  // Test members
};

TEST_F(SyncNewFeatureTest, FeatureScenario) {
  // Test implementation
  EXPECT_TRUE(condition);
}
```

### Adding Test Components

1. Add component definition to `test_components.hpp`
2. Implement serialization and equality
3. Add type name specialization
4. Update test templates to include new component

## Debugging Tests

### Verbose Output

```bash
meson test -C build --verbose
```

### GDB Debugging

```bash
gdb ./build/test/entt_ext_tests
(gdb) run --gtest_filter="SyncBasicTest.ComponentManagement"
```

### Logging

Tests use atomic counters and state tracking to verify behavior without relying on external logging.

## Known Limitations

1. **Network Testing**: Full network integration tests require more complex setup
2. **Performance Testing**: Current tests focus on correctness, not performance
3. **Stress Testing**: Multi-threaded stress tests not yet implemented
4. **Error Injection**: Network error simulation not implemented

## Future Improvements

- [ ] Add performance benchmarks
- [ ] Implement stress testing with multiple concurrent clients
- [ ] Add network error injection and recovery testing
- [ ] Create property-based testing for sync consistency
- [ ] Add memory leak detection
- [ ] Implement fuzzing tests for message handling
