#!/bin/bash

# Test runner script for entt_ext sync system tests

set -e

echo "Building entt_ext tests..."

# Build the project with tests
if [ ! -d "build" ]; then
    meson setup build
fi

meson compile -C build

echo "Running entt_ext sync tests..."

# Run the tests
meson test -C build --verbose

echo "Tests completed!"
