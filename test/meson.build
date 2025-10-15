# Test configuration for entt_ext

fs = import('fs')

gtest_dep = dependency('gtest', main: true, fallback: ['gtest', 'gtest_main_dep'])
gtest_main_dep = dependency('gtest', main: true, fallback: ['gtest', 'gtest_main_dep'])

# Test dependencies
spdlog_dep = dependency('spdlog', required: true)
fmt_dep = dependency('fmt', required: true)

# Try to get grlx dependency for sync tests
# Since we're in a subproject and grlx is also a subproject of the main project,
# we need to avoid the circular dependency by checking if grlx is available
# without creating a new subproject dependency
grlx_dep = dependency('', required: false)

# Check if we can find grlx headers directly
grlx_inc_path = '../../grlx/include'
if fs.is_dir(grlx_inc_path)
  # Create a grlx dependency using just the headers and required dependencies
  boost_dep = dependency('boost', required: true)
  openssl_dep = dependency('openssl', required: true)
  threads_dep = dependency('threads', required: true)
  curl_dep = dependency('libcurl', required: true)
  nlohmann_json_dep = dependency('nlohmann_json', required: true)
  
  grlx_dep = declare_dependency(
    include_directories: include_directories(grlx_inc_path),
    dependencies: [boost_dep, openssl_dep, threads_dep, curl_dep, nlohmann_json_dep]
  )
endif

test_deps = [
  entt_ext_dep,
  gtest_dep,
  gtest_main_dep,
  spdlog_dep,
  fmt_dep,
]

if grlx_dep.found()
  test_deps += [grlx_dep]
  message('grlx dependency found for sync tests')
else
  warning('grlx dependency not found - sync tests will be disabled')
endif

# Test source files
test_sources = [
  'test_ecs_basic.cpp',
]

# Add sync tests if grlx is available
if grlx_dep.found()
  test_sources += [
    'test_sync_basic.cpp',
    # Temporarily disabled until interface is fixed
    # 'test_sync_integration.cpp',
    # 'test_sync_loop_prevention.cpp',
    # 'test_sync_notifications.cpp',
  ]
endif

# Create test executable
test_exe = executable(
  'entt_ext_tests',
  test_sources,
  dependencies: test_deps,
  include_directories: include_directories('../include'),
  cpp_args: compiler_args,
  install: false
)

# Register tests
test('entt_ext_unit_tests', test_exe)
