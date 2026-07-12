#pragma once

// Generic preprocessor FOR_EACH: ENTT_EXT_FOR_EACH(action, a, b, c, ...)
// expands to `action(a) action(b) action(c) ...` for up to 64 comma-separated
// arguments. Standard C-preprocessor metaprogramming technique (argument-count
// dispatch via a reversed sequence + macro overload chain) — nothing
// entt_ext-specific here, it's a general-purpose utility.
//
// Why this exists: sync_client_shard.hpp / sync_server_shard.hpp use it to
// drive ENTT_EXT_SYNC_{CLIENT,SERVER}_{EXTERN,INSTANTIATE} across an entire
// component list from a single macro invocation, so an app's scaffold/shard
// .cpp files never need a per-component line — see those headers for the
// full per-app usage pattern. Adding/removing a component then only means
// editing the one comma-separated list in the app's sync_components.hpp.
//
// Argument splitting is purely paren-depth based (the preprocessor has no
// notion of C++ syntax), so this works fine for template-ids like
// `entt_ext::sync::server_only<foo::bar>` as long as no single argument
// contains an unparenthesized top-level comma — true for every wrapper type
// in entt_ext::sync (server_only<T>, with_hierarchy<T>, with_entity_refs<T>
// all take exactly one template argument).

#define ENTT_EXT_PP_EXPAND(x) x

#define ENTT_EXT_PP_CONCAT_(a, b) a##b
#define ENTT_EXT_PP_CONCAT(a, b) ENTT_EXT_PP_CONCAT_(a, b)

#define ENTT_EXT_PP_ARG_N( \
  _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
  _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
  _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
  _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
  _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
  _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
  _61, _62, _63, _64, N, ...) N

#define ENTT_EXT_PP_RSEQ_N() \
  64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, \
  48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, \
  32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, \
  16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1

#define ENTT_EXT_PP_NARG_(...) ENTT_EXT_PP_EXPAND(ENTT_EXT_PP_ARG_N(__VA_ARGS__))
#define ENTT_EXT_PP_NARG(...) ENTT_EXT_PP_NARG_(__VA_ARGS__, ENTT_EXT_PP_RSEQ_N())

#define ENTT_EXT_FOR_EACH_1(what, x) what(x)
#define ENTT_EXT_FOR_EACH_2(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_1(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_3(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_2(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_4(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_3(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_5(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_4(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_6(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_5(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_7(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_6(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_8(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_7(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_9(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_8(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_10(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_9(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_11(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_10(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_12(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_11(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_13(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_12(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_14(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_13(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_15(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_14(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_16(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_15(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_17(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_16(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_18(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_17(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_19(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_18(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_20(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_19(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_21(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_20(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_22(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_21(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_23(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_22(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_24(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_23(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_25(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_24(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_26(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_25(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_27(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_26(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_28(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_27(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_29(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_28(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_30(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_29(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_31(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_30(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_32(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_31(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_33(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_32(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_34(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_33(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_35(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_34(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_36(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_35(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_37(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_36(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_38(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_37(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_39(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_38(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_40(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_39(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_41(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_40(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_42(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_41(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_43(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_42(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_44(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_43(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_45(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_44(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_46(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_45(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_47(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_46(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_48(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_47(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_49(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_48(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_50(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_49(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_51(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_50(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_52(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_51(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_53(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_52(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_54(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_53(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_55(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_54(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_56(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_55(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_57(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_56(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_58(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_57(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_59(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_58(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_60(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_59(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_61(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_60(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_62(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_61(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_63(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_62(what, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_64(what, x, ...) what(x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_63(what, __VA_ARGS__))

#define ENTT_EXT_FOR_EACH_(N, what, ...) \
  ENTT_EXT_PP_EXPAND(ENTT_EXT_PP_CONCAT(ENTT_EXT_FOR_EACH_, N)(what, __VA_ARGS__))

// ENTT_EXT_FOR_EACH(action, a, b, c) -> action(a) action(b) action(c)
// Supports 1-64 arguments. Needs at least one argument (an empty group
// list isn't meaningful for our use case, and __VA_OPT__-based zero-arg
// support isn't worth the extra complexity here).
#define ENTT_EXT_FOR_EACH(what, ...) \
  ENTT_EXT_FOR_EACH_(ENTT_EXT_PP_NARG(__VA_ARGS__), what, __VA_ARGS__)

// ENTT_EXT_FOR_EACH_ARG(action, fixed, a, b, c) -> action(fixed, a) action(fixed, b) action(fixed, c)
// Same as ENTT_EXT_FOR_EACH, but threads one fixed leading argument through
// every call — lets a two-parameter action macro (e.g. one taking a type
// alias AND a component type) be driven by FOR_EACH without the caller
// having to hand-declare a temporary one-parameter binder macro first
// (which the preprocessor can't do anyway: a macro body can't contain a
// `#define` directive). Supports 1-64 trailing (variable) arguments.
#define ENTT_EXT_FOR_EACH_ARG_1(what, arg0, x) what(arg0, x)
#define ENTT_EXT_FOR_EACH_ARG_2(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_1(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_3(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_2(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_4(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_3(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_5(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_4(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_6(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_5(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_7(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_6(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_8(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_7(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_9(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_8(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_10(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_9(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_11(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_10(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_12(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_11(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_13(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_12(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_14(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_13(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_15(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_14(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_16(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_15(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_17(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_16(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_18(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_17(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_19(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_18(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_20(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_19(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_21(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_20(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_22(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_21(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_23(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_22(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_24(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_23(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_25(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_24(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_26(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_25(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_27(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_26(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_28(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_27(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_29(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_28(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_30(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_29(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_31(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_30(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_32(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_31(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_33(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_32(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_34(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_33(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_35(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_34(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_36(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_35(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_37(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_36(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_38(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_37(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_39(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_38(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_40(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_39(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_41(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_40(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_42(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_41(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_43(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_42(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_44(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_43(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_45(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_44(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_46(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_45(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_47(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_46(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_48(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_47(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_49(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_48(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_50(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_49(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_51(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_50(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_52(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_51(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_53(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_52(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_54(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_53(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_55(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_54(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_56(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_55(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_57(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_56(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_58(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_57(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_59(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_58(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_60(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_59(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_61(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_60(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_62(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_61(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_63(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_62(what, arg0, __VA_ARGS__))
#define ENTT_EXT_FOR_EACH_ARG_64(what, arg0, x, ...) what(arg0, x) ENTT_EXT_PP_EXPAND(ENTT_EXT_FOR_EACH_ARG_63(what, arg0, __VA_ARGS__))

#define ENTT_EXT_FOR_EACH_ARG_(N, what, arg0, ...) \
  ENTT_EXT_PP_EXPAND(ENTT_EXT_PP_CONCAT(ENTT_EXT_FOR_EACH_ARG_, N)(what, arg0, __VA_ARGS__))

#define ENTT_EXT_FOR_EACH_ARG(what, arg0, ...) \
  ENTT_EXT_FOR_EACH_ARG_(ENTT_EXT_PP_NARG(__VA_ARGS__), what, arg0, __VA_ARGS__)
