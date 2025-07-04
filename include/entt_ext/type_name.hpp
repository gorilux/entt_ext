#pragma once

#include <string_view>

// https://stackoverflow.com/questions/81870/is-it-possible-to-print-a-variables-type-in-standard-c
namespace entt_ext {
template <typename T>
constexpr auto type_name() {
  std::string_view name, prefix, suffix;
#ifdef __clang__
  name   = __PRETTY_FUNCTION__;
  prefix = "auto grlx::type_name() [T = ";
  suffix = "]";
#elif defined(__GNUC__)
  name   = __PRETTY_FUNCTION__;
  prefix = "constexpr auto grlx::type_name() [with T = ";
  suffix = "]";
#elif defined(_MSC_VER)
  name   = __FUNCSIG__;
  prefix = "auto __cdecl grlx::type_name<";
  suffix = ">(void)";
#endif
  name.remove_prefix(prefix.size());
  name.remove_suffix(suffix.size());
  return name;
}
} // namespace entt_ext