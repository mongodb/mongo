#ifndef BOOST_COMPAT_FUNCTION_REF_HPP_INCLUDED
#define BOOST_COMPAT_FUNCTION_REF_HPP_INCLUDED

// Copyright 2024 Christian Mazakas.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/compat/invoke.hpp>
#include <boost/compat/type_traits.hpp>

#include <type_traits>
#include <utility>

#if defined(__cpp_nontype_template_parameter_auto) && __cpp_nontype_template_parameter_auto >= 201606L
#define BOOST_COMPAT_HAS_AUTO_NTTP
#endif

namespace boost {
namespace compat {

template <class... S>
struct function_ref;

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

template <auto V>
struct nontype_t {
  explicit nontype_t() = default;
};

#endif

namespace detail {

template <bool NoEx>
union thunk_storage {
  void* pobj_;
  void (*pfn_)() noexcept(NoEx);
};

template <bool NoEx, class Fp, class R, class... Args>
struct invoke_function_holder {
  static R invoke_function(thunk_storage<NoEx> s, Args&&... args) noexcept(NoEx) {
    auto f = reinterpret_cast<Fp>(s.pfn_);
    return compat::invoke_r<R>(f, std::forward<Args>(args)...);
  }
};

template <bool Const, bool NoEx, class F, class R, class... Args>
struct invoke_object_holder {
  static R invoke_object(thunk_storage<NoEx> s, Args&&... args) noexcept(NoEx) {
    using T = remove_reference_t<F>;
    using cv_T = conditional_t<Const, add_const_t<T>, T>;
    return compat::invoke_r<R>(*static_cast<cv_T*>(s.pobj_), std::forward<Args>(args)...);
  }
};

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

template <auto f, bool Const, bool NoEx, class R, class... Args>
struct invoke_mem_fn_holder {
  static R invoke_mem_fn(thunk_storage<NoEx> /* s */, Args&&... args) noexcept(NoEx) {
    return compat::invoke_r<R>(f, std::forward<Args>(args)...);
  }
};

template <auto f, class U, bool Const, bool NoEx, class R, class... Args>
struct invoke_target_mem_fn_holder {
  static R invoke_mem_fn(thunk_storage<NoEx> s, Args&&... args) noexcept(NoEx) {
    using T = remove_reference_t<U>;
    using cv_T = conditional_t<Const, add_const_t<T>, T>;
    return compat::invoke_r<R>(f, *static_cast<cv_T*>(s.pobj_), std::forward<Args>(args)...);
  }
};

template <auto f, class T, bool Const, bool NoEx, class R, class... Args>
struct invoke_ptr_mem_fn_holder {
  static R invoke_mem_fn(thunk_storage<NoEx> s, Args&&... args) noexcept(NoEx) {
    using cv_T = conditional_t<Const, add_const_t<T>, T>;
    return compat::invoke_r<R>(f, static_cast<cv_T*>(s.pobj_), std::forward<Args>(args)...);
  }
};

#endif

template <bool Const, bool NoEx, class R, class... Args>
struct function_ref_base {
private:
  thunk_storage<NoEx> thunk_ = nullptr;
  R (*invoke_)(thunk_storage<NoEx>, Args&&...) noexcept(NoEx) = nullptr;

public:
  struct fp_tag {};
  struct obj_tag {};
  struct mem_fn_tag {};

  template <class F>
  function_ref_base(fp_tag, F* fn) noexcept
      : thunk_{}, invoke_(&invoke_function_holder<NoEx, F*, R, Args...>::invoke_function) {
    thunk_.pfn_ = reinterpret_cast<decltype(thunk_.pfn_)>(fn);
  }

  template <class F>
  function_ref_base(obj_tag, F&& fn) noexcept
      : thunk_{}, invoke_(&invoke_object_holder<Const, NoEx, F, R, Args...>::invoke_object) {
    thunk_.pobj_ = const_cast<void*>(static_cast<void const*>(std::addressof(fn)));
  }

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

  template <auto f, class F = decltype(f)>
  function_ref_base(mem_fn_tag, nontype_t<f>)
      : thunk_{}, invoke_(&invoke_mem_fn_holder<F{f}, Const, NoEx, R, Args...>::invoke_mem_fn) {
    thunk_.pobj_ = nullptr;
  }

  template <auto f, class U, class F = decltype(f)>
  function_ref_base(mem_fn_tag, nontype_t<f>, U&& obj)
      : thunk_{}, invoke_(&invoke_target_mem_fn_holder<F{f}, U, Const, NoEx, R, Args...>::invoke_mem_fn) {
    thunk_.pobj_ = const_cast<void*>(static_cast<void const*>(std::addressof(obj)));
  }

  template <auto f, class T, class F = decltype(f)>
  function_ref_base(mem_fn_tag, nontype_t<f>, T* obj)
      : thunk_{}, invoke_(&invoke_ptr_mem_fn_holder<F{f}, T, Const, NoEx, R, Args...>::invoke_mem_fn) {
    thunk_.pobj_ = const_cast<void*>(static_cast<void const*>(obj));
  }

#endif

  function_ref_base(const function_ref_base&) noexcept = default;
  function_ref_base& operator=(const function_ref_base&) noexcept = default;

  R operator()(Args... args) const noexcept(NoEx) { return this->invoke_(thunk_, std::forward<Args>(args)...); }
};

} // namespace detail

template <class R, class... Args>
struct function_ref<R(Args...)> : public detail::function_ref_base<false, false, R, Args...> {
private:
  using base_type = detail::function_ref_base<false, false, R, Args...>;
  using typename base_type::fp_tag;
  using typename base_type::mem_fn_tag;
  using typename base_type::obj_tag;

  template <class... T>
  using is_invocable_using = boost::compat::is_invocable_r<R, T..., Args...>;

public:
  template <class F, enable_if_t<std::is_function<F>::value && is_invocable_using<F>::value, int> = 0>
  function_ref(F* fn) noexcept : base_type(fp_tag{}, fn) {}

  template <class F, class T = remove_reference_t<F>,
            enable_if_t<!std::is_same<remove_cvref_t<F>, function_ref>::value && !std::is_member_pointer<T>::value &&
                            is_invocable_using<T&>::value,
                        int> = 0>
  function_ref(F&& fn) noexcept : base_type(obj_tag{}, fn) {}

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

  template <auto f, class F = decltype(f), enable_if_t<is_invocable_using<F>::value, int> = 0>
  function_ref(nontype_t<f> x) noexcept : base_type(mem_fn_tag{}, x) {}

  template <auto f, class U, class T = remove_reference_t<U>, class F = decltype(f),
            enable_if_t<!std::is_rvalue_reference_v<U&&> && is_invocable_using<F, T&>::value, int> = 0>
  function_ref(nontype_t<f> x, U&& obj) noexcept : base_type(mem_fn_tag{}, x, std::forward<U>(obj)) {}

  template <auto f, class T, class F = decltype(f), enable_if_t<is_invocable_using<F, T*>::value, int> = 0>
  function_ref(nontype_t<f> x, T* obj) noexcept : base_type(mem_fn_tag{}, x, obj) {}

#endif

  function_ref(const function_ref&) noexcept = default;
  function_ref& operator=(const function_ref&) noexcept = default;

  template <class T, enable_if_t<!std::is_same<T, function_ref>::value && !std::is_pointer<T>::value, int> = 0>
  function_ref& operator=(T) = delete;
};

template <class R, class... Args>
struct function_ref<R(Args...) const> : public detail::function_ref_base<true, false, R, Args...> {
private:
  using base_type = detail::function_ref_base<true, false, R, Args...>;
  using typename base_type::fp_tag;
  using typename base_type::mem_fn_tag;
  using typename base_type::obj_tag;

  template <class... T>
  using is_invocable_using = boost::compat::is_invocable_r<R, T..., Args...>;

public:
  template <class F, enable_if_t<std::is_function<F>::value && is_invocable_using<F>::value, int> = 0>
  function_ref(F* fn) noexcept : base_type(fp_tag{}, fn) {}

  template <class F, class T = remove_reference_t<F>,
            enable_if_t<!std::is_same<remove_cvref_t<F>, function_ref>::value && !std::is_member_pointer<T>::value &&
                            is_invocable_using<T const&>::value,
                        int> = 0>
  function_ref(F&& fn) noexcept : base_type(obj_tag{}, fn) {}

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

  template <auto f, class F = decltype(f), enable_if_t<is_invocable_using<F>::value, int> = 0>
  function_ref(nontype_t<f> x) noexcept : base_type(mem_fn_tag{}, x) {}

  template <auto f, class U, class T = remove_reference_t<U>, class F = decltype(f),
            enable_if_t<!std::is_rvalue_reference_v<U&&> && is_invocable_using<F, T const&>::value, int> = 0>
  function_ref(nontype_t<f> x, U&& obj) noexcept : base_type(mem_fn_tag{}, x, std::forward<U>(obj)) {}

  template <auto f, class T, class F = decltype(f), enable_if_t<is_invocable_using<F, T const*>::value, int> = 0>
  function_ref(nontype_t<f> x, T const* obj) noexcept : base_type(mem_fn_tag{}, x, obj) {}

#endif

  function_ref(const function_ref&) noexcept = default;
  function_ref& operator=(const function_ref&) noexcept = default;

  template <class T, enable_if_t<!std::is_same<T, function_ref>::value && !std::is_pointer<T>::value, int> = 0>
  function_ref& operator=(T) = delete;
};

#if defined(__cpp_noexcept_function_type)

template <class R, class... Args>
struct function_ref<R(Args...) noexcept> : public detail::function_ref_base<false, true, R, Args...> {
private:
  using base_type = detail::function_ref_base<false, true, R, Args...>;
  using typename base_type::fp_tag;
  using typename base_type::mem_fn_tag;
  using typename base_type::obj_tag;

  template <class... T>
  using is_invocable_using = boost::compat::is_nothrow_invocable_r<R, T..., Args...>;

public:
  template <class F, enable_if_t<std::is_function<F>::value && is_invocable_using<F>::value, int> = 0>
  function_ref(F* fn) noexcept : base_type(fp_tag{}, fn) {}

  template <class F, class T = remove_reference_t<F>,
            enable_if_t<!std::is_same<remove_cvref_t<F>, function_ref>::value && !std::is_member_pointer<T>::value &&
                            is_invocable_using<T&>::value,
                        int> = 0>
  function_ref(F&& fn) noexcept : base_type(obj_tag{}, fn) {}

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

  template <auto f, class F = decltype(f), enable_if_t<is_invocable_using<F>::value, int> = 0>
  function_ref(nontype_t<f> x) noexcept : base_type(mem_fn_tag{}, x) {}

  template <auto f, class U, class T = remove_reference_t<U>, class F = decltype(f),
            enable_if_t<!std::is_rvalue_reference_v<U&&> && is_invocable_using<F, T&>::value, int> = 0>
  function_ref(nontype_t<f> x, U&& obj) noexcept : base_type(mem_fn_tag{}, x, std::forward<U>(obj)) {}

  template <auto f, class T, class F = decltype(f), enable_if_t<is_invocable_using<F, T*>::value, int> = 0>
  function_ref(nontype_t<f> x, T* obj) noexcept : base_type(mem_fn_tag{}, x, obj) {}

#endif

  function_ref(const function_ref&) noexcept = default;
  function_ref& operator=(const function_ref&) noexcept = default;

  template <class T, enable_if_t<!std::is_same<T, function_ref>::value && !std::is_pointer<T>::value, int> = 0>
  function_ref& operator=(T) = delete;
};

template <class R, class... Args>
struct function_ref<R(Args...) const noexcept> : public detail::function_ref_base<true, true, R, Args...> {
private:
  using base_type = detail::function_ref_base<true, true, R, Args...>;
  using typename base_type::fp_tag;
  using typename base_type::mem_fn_tag;
  using typename base_type::obj_tag;

  template <class... T>
  using is_invocable_using = boost::compat::is_nothrow_invocable_r<R, T..., Args...>;

public:
  template <class F, enable_if_t<std::is_function<F>::value && is_invocable_using<F>::value, int> = 0>
  function_ref(F* fn) noexcept : base_type(fp_tag{}, fn) {}

  template <class F, class T = remove_reference_t<F>,
            enable_if_t<!std::is_same<remove_cvref_t<F>, function_ref>::value && !std::is_member_pointer<T>::value &&
                            is_invocable_using<T const&>::value,
                        int> = 0>
  function_ref(F&& fn) noexcept : base_type(obj_tag{}, fn) {}

#if defined(BOOST_COMPAT_HAS_AUTO_NTTP)

  template <auto f, class F = decltype(f), enable_if_t<is_invocable_using<F>::value, int> = 0>
  function_ref(nontype_t<f> x) noexcept : base_type(mem_fn_tag{}, x) {}

  template <auto f, class U, class T = remove_reference_t<U>, class F = decltype(f),
            enable_if_t<!std::is_rvalue_reference_v<U&&> && is_invocable_using<F, T const&>::value, int> = 0>
  function_ref(nontype_t<f> x, U&& obj) noexcept : base_type(mem_fn_tag{}, x, std::forward<U>(obj)) {}

  template <auto f, class T, class F = decltype(f), enable_if_t<is_invocable_using<F, T const*>::value, int> = 0>
  function_ref(nontype_t<f> x, T const* obj) noexcept : base_type(mem_fn_tag{}, x, obj) {}

#endif

  function_ref(const function_ref&) noexcept = default;
  function_ref& operator=(const function_ref&) noexcept = default;

  template <class T, enable_if_t<!std::is_same<T, function_ref>::value && !std::is_pointer<T>::value, int> = 0>
  function_ref& operator=(T) = delete;
};

#endif

} // namespace compat
} // namespace boost

#endif // #ifndef BOOST_COMPAT_FUNCTION_REF_HPP_INCLUDED
