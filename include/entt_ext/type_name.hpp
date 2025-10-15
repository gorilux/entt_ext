#pragma once

#include <string_view>

// https://stackoverflow.com/questions/81870/is-it-possible-to-print-a-variables-type-in-standard-c
namespace entt_ext {
template <typename T>
constexpr auto type_name() {
  std::string_view name, prefix, suffix;
#ifdef __clang__
  name   = __PRETTY_FUNCTION__;
  prefix = "auto entt_ext::type_name() [T = ";
  suffix = "]";
#elif defined(__GNUC__)
  name   = __PRETTY_FUNCTION__;
  prefix = "constexpr auto entt_ext::type_name() [with T = ";
  suffix = "]";
#elif defined(_MSC_VER)
  name   = __FUNCSIG__;
  prefix = "auto __cdecl entt_ext::type_name<";
  suffix = ">(void)";
#endif
  name.remove_prefix(prefix.size());
  name.remove_suffix(suffix.size());

  // Remove "const " prefix if present
  constexpr std::string_view const_prefix = "const ";
  if (name.starts_with(const_prefix)) {
    name.remove_prefix(const_prefix.size());
  }

  return name;
}
} // namespace entt_ext