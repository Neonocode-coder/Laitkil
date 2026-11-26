// =============================================================================
//  LKLBridge.hpp - удобная C++17-обёртка над LKLBridge.h
//
//  Обычные C++ функции и лямбды регистрируются одной строкой: сигнатура для
//  скрипта строится из типов C++ автоматически.
//
//      lkl::State lk;
//      lk.bind("add", [](double a, double b) { return a + b; });
//      lk.bind("greet", [](const std::string& who) { return "Hello, " + who; });
//      lk.doString("print(add(2, 3), greet(\"мир\"))");
//
//      lk.doString("func:num sq(num x) { give x * x }");
//      double r = lk.call<double>("sq", 7);        // 49
//
//  Соответствие типов:
//      double, float, int, long, ...  <-> num       bool                  <-> bool
//      std::string                    <-> str       std::vector<T>        <-> a_<T>
//      std::map<std::string, T>       <-> d_<T>     void                  - без результата
//  Ошибки скрипта - исключение lkl::Error; exit(код) в скрипте - lkl::Exit.
//  Исключения, брошенные внутри привязанных функций, становятся ошибками скрипта.
// =============================================================================
#ifndef LKLBRIDGE_HPP
#define LKLBRIDGE_HPP

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "LKLBridge.h"

namespace lkl {

struct Error : std::runtime_error {                       // ошибка компиляции/выполнения/API
  int code;                                               // LKL_ERR_*
  Error(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};
struct Exit { int code; };                                // скрипт вызвал exit(code)

// ---------------------------------------------------------------------------
// Соответствие C++ типов и типов Laitkil
// ---------------------------------------------------------------------------
template <class T, class = void> struct Traits;

template <class T>
struct Traits<T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>> {
  static std::string name() { return "num"; }
  static void push(lkl_State* L, T v) { lkl_pushnum(L, static_cast<double>(v)); }
  static T get(lkl_State* L, int idx) { return static_cast<T>(lkl_tonum(L, idx)); }
};
template <> struct Traits<bool> {
  static std::string name() { return "bool"; }
  static void push(lkl_State* L, bool v) { lkl_pushbool(L, v); }
  static bool get(lkl_State* L, int idx) { return lkl_tobool(L, idx) != 0; }
};
template <> struct Traits<std::string> {
  static std::string name() { return "str"; }
  static void push(lkl_State* L, const std::string& v) { lkl_pushlstring(L, v.data(), v.size()); }
  static std::string get(lkl_State* L, int idx) {
    size_t n = 0; const char* s = lkl_tostring(L, idx, &n);
    return s ? std::string(s, n) : std::string();
  }
};
template <> struct Traits<const char*> {                  // только передача в скрипт
  static std::string name() { return "str"; }
  static void push(lkl_State* L, const char* v) { lkl_pushstring(L, v); }
};
template <> struct Traits<std::string_view> {             // только передача в скрипт
  static std::string name() { return "str"; }
  static void push(lkl_State* L, std::string_view v) { lkl_pushlstring(L, v.data(), v.size()); }
};
template <class T> struct Traits<std::vector<T>> {
  static std::string name() { return "a_" + Traits<T>::name(); }
  static void push(lkl_State* L, const std::vector<T>& v) {
    lkl_pusharray(L, Traits<T>::name().c_str());
    for (const T& x : v) { Traits<T>::push(L, x); lkl_arrayappend(L, -2); }
  }
  static std::vector<T> get(lkl_State* L, int idx) {
    idx = lkl_absindex(L, idx);
    std::vector<T> out;
    long n = lkl_arraylen(L, idx);
    for (long i = 0; i < n; i++) {
      lkl_arrayget(L, idx, (size_t)i);
      out.push_back(Traits<T>::get(L, -1));
      lkl_pop(L, 1);
    }
    return out;
  }
};
template <class T> struct Traits<std::map<std::string, T>> {
  static std::string name() { return "d_" + Traits<T>::name(); }
  static void push(lkl_State* L, const std::map<std::string, T>& m) {
    lkl_pushdict(L, Traits<T>::name().c_str());
    for (auto& kv : m) { Traits<T>::push(L, kv.second); lkl_dictset(L, -2, kv.first.c_str()); }
  }
  static std::map<std::string, T> get(lkl_State* L, int idx) {
    idx = lkl_absindex(L, idx);
    std::map<std::string, T> out;
    for (size_t i = 0; lkl_dictentry(L, idx, i) == 1; i++) {
      out[Traits<std::string>::get(L, -2)] = Traits<T>::get(L, -1);
      lkl_pop(L, 2);
    }
    return out;
  }
};

namespace detail {
template <class F> struct FnTraits : FnTraits<decltype(&F::operator())> {};
template <class R, class... A> struct FnTraits<R (*)(A...)> {
  using Ret = R; using Args = std::tuple<std::decay_t<A>...>;
};
template <class R, class... A> struct FnTraits<R(A...)> : FnTraits<R (*)(A...)> {};
template <class C, class R, class... A> struct FnTraits<R (C::*)(A...) const> : FnTraits<R (*)(A...)> {};
template <class C, class R, class... A> struct FnTraits<R (C::*)(A...)> : FnTraits<R (*)(A...)> {};

template <class Tuple> struct SigOf;
template <class... A> struct SigOf<std::tuple<A...>> {
  static std::string params() {
    std::string s; ((s += (s.empty() ? "" : " ") + Traits<A>::name()), ...); return s;
  }
};

template <class F, class Ret, class Args, class Seq> struct Binder;
template <class F, class Ret, class... A, size_t... I>
struct Binder<F, Ret, std::tuple<A...>, std::index_sequence<I...>> {
  F f;
  static int call(lkl_State* L) {                         // C-трамплин, вызывается из VM
    auto* self = static_cast<Binder*>(lkl_getuserdata(L));
    try {
      if constexpr (std::is_void_v<Ret>) { self->f(Traits<A>::get(L, int(I) + 1)...); return 0; }
      else { Traits<Ret>::push(L, self->f(Traits<A>::get(L, int(I) + 1)...)); return 1; }
    } catch (const std::exception& e) { return lkl_error(L, "%s", e.what()); }
    catch (...) { return lkl_error(L, "unknown C++ exception"); }
  }
  static void destroy(void* p) { delete static_cast<Binder*>(p); }
};
}  // namespace detail

// ---------------------------------------------------------------------------
// State - RAII-владелец lkl_State
// ---------------------------------------------------------------------------
class State {
 public:
  explicit State(size_t vmStackSlots = 0) : L_(vmStackSlots ? lkl_newstateex(vmStackSlots) : lkl_newstate()) {
    if (!L_) throw std::bad_alloc();
  }
  ~State() { if (L_) lkl_close(L_); }
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&& o) noexcept : L_(o.L_), writer_(std::move(o.writer_)) { o.L_ = nullptr; rebindWriter(); }
  State& operator=(State&& o) noexcept {
    if (this != &o) { if (L_) lkl_close(L_); L_ = o.L_; writer_ = std::move(o.writer_); o.L_ = nullptr; rebindWriter(); }
    return *this;
  }
  lkl_State* raw() const { return L_; }                   // для вызовов низкоуровневого C API

  // ----- запуск кода -----
  void doString(const std::string& src) { check(lkl_dobuffer(L_, src.data(), src.size())); }
  void doFile(const std::string& path) { check(lkl_dofile(L_, path.c_str())); }

  // ----- C++ функция/лямбда -> функция скрипта -----
  template <class F> void bind(const std::string& name, F&& fn) {
    using Fn = std::decay_t<F>;
    using Tr = detail::FnTraits<Fn>;
    using Args = typename Tr::Args;
    using Ret = typename Tr::Ret;
    using B = detail::Binder<Fn, Ret, Args, std::make_index_sequence<std::tuple_size_v<Args>>>;
    std::string sig = detail::SigOf<Args>::params();
    if constexpr (!std::is_void_v<Ret>) sig += " -> " + Traits<Ret>::name();
    auto* b = new B{std::forward<F>(fn)};
    check(lkl_registerx(L_, name.c_str(), &B::call, sig.c_str(), b, &B::destroy));
  }

  // ----- вызов функций скрипта из C++ -----
  template <class R = void, class... Args> R call(const std::string& fn, const Args&... args) {
    int base = lkl_gettop(L_);
    (Traits<std::decay_t<Args>>::push(L_, args), ...);
    int rc = lkl_call(L_, fn.c_str(), int(sizeof...(Args)));
    if (rc != LKL_OK) { lkl_settop(L_, base); check(rc); }
    return takeResult<R>(base, fn);
  }

  // ----- глобальные переменные -----
  template <class T> void setGlobal(const std::string& name, const T& v) {
    Traits<std::decay_t<T>>::push(L_, v);
    check(lkl_setglobal(L_, name.c_str()));
  }
  template <class T> T getGlobal(const std::string& name) {
    int base = lkl_gettop(L_);
    check(lkl_getglobal(L_, name.c_str()));
    T r = Traits<T>::get(L_, -1);
    lkl_settop(L_, base);
    return r;
  }

  // ----- вывод print/write -----
  void setWriter(std::function<void(std::string_view)> w) { writer_ = std::move(w); rebindWriter(); }

 private:
  lkl_State* L_;
  std::function<void(std::string_view)> writer_;

  void rebindWriter() {
    if (!L_) return;
    if (!writer_) { lkl_setwriter(L_, nullptr, nullptr); return; }
    lkl_setwriter(L_, [](const char* d, size_t n, void* ud) {
      (*static_cast<std::function<void(std::string_view)>*>(ud))(std::string_view(d, n));
    }, &writer_);
  }
  void check(int rc) {
    if (rc == LKL_OK) return;
    if (rc == LKL_EXIT) throw Exit{lkl_exitcode(L_)};
    throw Error(rc, lkl_geterror(L_));
  }
  template <class R> R takeResult(int base, const std::string& fn) {
    if constexpr (std::is_void_v<R>) { lkl_settop(L_, base); }
    else {
      if (lkl_gettop(L_) != base + 1) throw Error(LKL_ERR_API, "function '" + fn + "' returns no value");
      if (lkl_typename(L_, -1) != Traits<R>::name()) {
        std::string got = lkl_typename(L_, -1); lkl_settop(L_, base);
        throw Error(LKL_ERR_API, "function '" + fn + "' returns '" + got + "', requested '" + Traits<R>::name() + "'");
      }
      R r = Traits<R>::get(L_, -1);
      lkl_settop(L_, base);
      return r;
    }
  }
};

}  // namespace lkl
#endif  // LKLBRIDGE_HPP
