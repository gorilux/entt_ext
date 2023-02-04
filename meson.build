project('entt_ext', 'cpp', version : '0.0.1', license: 'MIT', default_options : ['cpp_std=c++23'])

# Add Boost dependency
boost_dep = dependency('boost')

entt_ext_sources = [  
  'include/entt_ext/ecs.hpp',
  'include/entt_ext/core.hpp',
  'include/entt_ext/nodes.hpp',
  'include/entt_ext/settings.hpp',
  'src/ecs.cpp'
]

cpp = meson.get_compiler('cpp')
compiler_args = ['-DENTT_USE_ATOMIC=1', '-DENTT_ID_TYPE=std::uint64_t']
linker_args = []
system = host_machine.system()

cereal_project = subproject('cereal')
cereal_dep = cereal_project.get_variable('cereal_dep')

# libgrlx_project = subproject('grlx')
# grlx_dep  = libgrlx_project.get_variable('grlx_dep')


cmake = import('cmake')

entt_opt_var = cmake.subproject_options()
entt_proj = cmake.subproject('entt', options: entt_opt_var)
message('CMaket targets:\n - ' + '\n - '.join(entt_proj.target_list()))
entt_proj_dep = entt_proj.dependency('EnTT')

entt_ext_deps = [
  cereal_dep,    
  entt_proj_dep,
  boost_dep
]

if system == 'emscripten'
  compiler_args += [
  '-s', 'USE_BOOST_HEADERS=1',
  '-s', 'DISABLE_EXCEPTION_CATCHING=1', 
  '-DBOOST_ASIO_HAS_STD_INVOKE_RESULT=1'
  ]  
  
else  

endif

entt_ext_includes = include_directories('./include')

entt_ext_library = static_library( 'entt_ext',
  sources: entt_ext_sources,
  dependencies: entt_ext_deps,  
  include_directories : [ entt_ext_includes , './include/entt_ext' ],
  c_args:compiler_args, 
  cpp_args:compiler_args,  
)

### gnu_symbol_visibility : 'hidden',

#Make this library usable as a Meson subproject.
entt_ext_dep = declare_dependency(
  include_directories: entt_ext_includes,
  dependencies: entt_ext_deps,  
  link_with : entt_ext_library,
  compile_args: compiler_args,
  link_args:linker_args
)

set_variable('entt_ext_dep', entt_ext_dep)



# Make this library usable from the system's
# package manager.
# install_headers(grlx_headers, subdir : meson.project_name())

# pkg_mod = import('pkgconfig')
# pkg_mod.generate(
#   name : meson.project_name(),
#   filebase : meson.project_name(),
#   description : project_description,
#   subdirs : meson.project_name(),
# )


# ==========
# Unit Tests
# ==========

if not meson.is_subproject() and system != 'emscripten'
#   add_languages('cpp')
#  subdir('test')

#   test('all_tests',
#     executable(
#       'run_tests',
#       files(project_test_files),
#       dependencies : [grlx_dep, test_dep],
#       install : false
#     )
#   )
endif

