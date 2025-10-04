//
// immer: immutable data structures for C++
// Copyright (C) 2016, 2017, 2018 Juan Pedro Bolivar Puente
//
// This software is distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://boost.org/LICENSE_1_0.txt
//

#pragma once

#include <immer/detail/util.hpp>
#include <immer/memory_policy.hpp>

#include <cstddef>

namespace immer {

namespace detail {

template <typename U, typename MP>
struct gc_atom_impl;

template <typename U, typename MP>
struct refcount_atom_impl;

} // namespace detail

/*!
 * Immutable box for a single value of type `T`.
 *
 * The box is always copiable and movable. The `T` copy or move
 * operations are never called.  Since a box is immutable, copying or
 * moving just copy the underlying pointers.
 */
template <typename T, typename MemoryPolicy = default_memory_policy>
class box
{
    friend struct detail::gc_atom_impl<T, MemoryPolicy>;
    friend struct detail::refcount_atom_impl<T, MemoryPolicy>;

    struct holder : MemoryPolicy::refcount
    {
        T value;

        template <typename... Args>
        holder(Args&&... args)
            : value{std::forward<Args>(args)...}
        {}
    };

    using heap = typename MemoryPolicy::heap::type;

    holder* impl_ = nullptr;

    box(holder* impl)
        : impl_{impl}
    {}

public:
    const holder* impl() const { return impl_; };

    using value_type    = T;
    using memory_policy = MemoryPolicy;

    /*!
     * Constructs a box holding `T{}`.
     */
    box()
        : impl_{detail::make<heap, holder>()}
    {}

    /*!
     * Constructs a box holding `T{arg}`
     */
    template <typename Arg,
              typename Enable = std::enable_if_t<
                  !std::is_same<box, std::decay_t<Arg>>::value &&
                  std::is_constructible<T, Arg>::value>>
    box(Arg&& arg)
        : impl_{detail::make<heap, holder>(std::forward<Arg>(arg))}
    {}

    /*!
     * Constructs a box holding `T{arg1, arg2, args...}`
     */
    template <typename Arg1, typename Arg2, typename... Args>
    box(Arg1&& arg1, Arg2&& arg2, Args&&... args)
        : impl_{detail::make<heap, holder>(std::forward<Arg1>(arg1),
                                           std::forward<Arg2>(arg2),
                                           std::forward<Args>(args)...)}
    {}

    friend void swap(box& a, box& b)
    {
        using std::swap;
        swap(a.impl_, b.impl_);
    }

    box(box&& other) { swap(*this, other); }
    box(const box& other)
        : impl_(other.impl_)
    {
        impl_->inc();
    }
    box& operator=(box&& other)
    {
        swap(*this, other);
        return *this;
    }
    box& operator=(const box& other)
    {
        auto aux = other;
        swap(*this, aux);
        return *this;
    }
    ~box()
    {
        if (impl_ && impl_->dec()) {
            impl_->~holder();
            heap::deallocate(sizeof(holder), impl_);
        }
    }

    /*! Query the current value. */
    IMMER_NODISCARD const T& get() const { return impl_->value; }

    /*! Conversion to the boxed type. */
    operator const T&() const { return get(); }

    /*! Access via dereference */
    const T& operator*() const { return get(); }

    /*! Access via pointer member access */
    const T* operator->() const { return &get(); }

    /*!
     * Returns a new box built by applying the `fn` to the underlying
     * value.
     *
     * @rst
     *
     * **Example**
     *   .. literalinclude:: ../example/box/box.cpp
     *      :language: c++
     *      :dedent: 8
     *      :start-after: update/start
     *      :end-before:  update/end
     *
     * @endrst
     */
    template <typename Fn>
    IMMER_NODISCARD box update(Fn&& fn) const&
    {
        return std::forward<Fn>(fn)(get());
    }
    template <typename Fn>
    IMMER_NODISCARD box&& update(Fn&& fn) &&
    {
        if (impl_->unique())
            impl_->value = std::forward<Fn>(fn)(std::move(impl_->value));
        else
            *this = std::forward<Fn>(fn)(impl_->value);
        return std::move(*this);
    }
};

template <typename T, typename MP>
IMMER_NODISCARD bool operator==(const box<T, MP>& a, const box<T, MP>& b)
{
    return a.impl() == b.impl() || a.get() == b.get();
}
template <typename T, typename MP>
IMMER_NODISCARD bool operator!=(const box<T, MP>& a, const box<T, MP>& b)
{
    return a.impl() != b.impl() && a.get() != b.get();
}
template <typename T, typename MP>
IMMER_NODISCARD bool operator<(const box<T, MP>& a, const box<T, MP>& b)
{
    return a.impl() != b.impl() && a.get() < b.get();
}

template <typename T, typename MP, typename T2>
IMMER_NODISCARD auto operator==(const box<T, MP>& a, T2&& b)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(a.get() == b)>
{
    return a.get() == b;
}
template <typename T, typename MP, typename T2>
IMMER_NODISCARD auto operator!=(const box<T, MP>& a, T2&& b)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(a.get() != b)>
{
    return a.get() != b;
}
template <typename T, typename MP, typename T2>
IMMER_NODISCARD auto operator<(const box<T, MP>& a, T2&& b)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(a.get() < b)>
{
    return a.get() < b;
}

template <typename T2, typename T, typename MP>
IMMER_NODISCARD auto operator==(T2&& b, const box<T, MP>& a)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(a.get() == b)>
{
    return a.get() == b;
}
template <typename T2, typename T, typename MP>
IMMER_NODISCARD auto operator!=(T2&& b, const box<T, MP>& a)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(a.get() != b)>
{
    return a.get() != b;
}
template <typename T2, typename T, typename MP>
IMMER_NODISCARD auto operator<(T2&& b, const box<T, MP>& a)
    -> std::enable_if_t<!std::is_same<box<T, MP>, std::decay_t<T2>>::value,
                        decltype(b < a.get())>
{
    return b < a.get();
}

} // namespace immer

namespace std {

template <typename T, typename MP>
struct hash<immer::box<T, MP>>
{
    std::size_t operator()(const immer::box<T, MP>& x) const
    {
        return std::hash<T>{}(*x);
    }
};

} // namespace std
