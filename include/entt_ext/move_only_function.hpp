// Polyfill for std::move_only_function (C++23, P0288R9).
// Used when the standard library does not yet provide it (e.g. Android NDK libc++).
// Only the subset actually used in entt_ext is implemented:
//   - move_only_function<R(Args...)>  (no const / noexcept qualifiers)

#pragma once

#include <functional> // pull in the real header first

#if __has_include(<version>)
#include <version>
#endif

// If the standard library already provides move_only_function, do nothing.
#if !defined(__cpp_lib_move_only_function)

#include <memory>
#include <type_traits>
#include <utility>

namespace std {

template <typename>
class move_only_function; // primary template — intentionally undefined

template <typename R, typename... Args>
class move_only_function<R(Args...)> {
  // Type-erased callable holder
  struct concept_t {
    virtual ~concept_t()                  = default;
    virtual R invoke(Args... args)        = 0;
  };

  template <typename F>
  struct model_t final : concept_t {
    F func;

    template <typename U>
    explicit model_t(U&& f) : func(std::forward<U>(f)) {}

    R invoke(Args... args) override {
      return func(std::forward<Args>(args)...);
    }
  };

  std::unique_ptr<concept_t> impl_;

public:
  move_only_function() noexcept = default;
  move_only_function(std::nullptr_t) noexcept {}

  // Construct from any callable that is not a move_only_function itself
  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, move_only_function> &&
                std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
  move_only_function(F&& f)
      : impl_(std::make_unique<model_t<std::decay_t<F>>>(std::forward<F>(f))) {}

  move_only_function(move_only_function&&) noexcept            = default;
  move_only_function& operator=(move_only_function&&) noexcept = default;

  // Not copyable
  move_only_function(const move_only_function&)            = delete;
  move_only_function& operator=(const move_only_function&) = delete;

  ~move_only_function() = default;

  R operator()(Args... args) {
    return impl_->invoke(std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  move_only_function& operator=(std::nullptr_t) noexcept {
    impl_.reset();
    return *this;
  }
};

} // namespace std

#endif // !defined(__cpp_lib_move_only_function)

