// Copyright Antony Polukhin, 2020-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// See http://www.boost.org/libs/any for Documentation.

#ifndef BOOST_ANYS_UNIQUE_ANY_HPP_INCLUDED
#define BOOST_ANYS_UNIQUE_ANY_HPP_INCLUDED

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif

/// \file boost/any/unique_any.hpp
/// \brief \copybrief boost::anys::unique_any

#include <memory>  // for std::unique_ptr
#include <utility>
#include <type_traits>

#include <boost/any/fwd.hpp>
#include <boost/any/bad_any_cast.hpp>
#include <boost/any/detail/placeholder.hpp>

#include <boost/throw_exception.hpp>
#include <boost/type_index.hpp>

namespace boost { namespace anys {

/// Helper type for providing emplacement type to the constructor.
template <class T>
struct in_place_type_t
{
};

#if !defined(BOOST_NO_CXX14_VARIABLE_TEMPLATES)
template <class T>
constexpr in_place_type_t<T> in_place_type{};
#endif

/// \brief A class whose instances can hold instances of any
/// type (including non-copyable and non-movable types).
class unique_any {
public:
    /// \post this->has_value() is false.
    constexpr unique_any() noexcept = default;

    /// Move constructor that moves content of
    /// `other` into new instance and leaves `other` empty.
    ///
    /// \post other->has_value() is false.
    /// \throws Nothing.
    unique_any(unique_any&& other) noexcept = default;

    /// Forwards `value`, so
    /// that the content of the new instance has type `std::decay_t<T>`
    /// and value is the `value` before the forward.
    ///
    /// \throws std::bad_alloc or any exceptions arising from the move or
    /// copy constructor of the contained type.
    template<typename T>
    unique_any(T&& value, typename std::enable_if<!std::is_same<T&&, boost::any&&>::value>::type* = nullptr)
      : content(new holder< typename std::decay<T>::type >(std::forward<T>(value)))
    {
        static_assert(
            !boost::anys::detail::is_basic_any< typename std::decay<T>::type >::value,
            "boost::anys::unique_any could not be constructed from boost::anys::basic_any."
        );

        static_assert(
            !std::is_same<unique_any, typename std::decay<T>::type >::value,
            "boost::anys::unique_any could not be copied, only moved."
        );

        static_assert(
            !std::is_same<boost::any, typename std::decay<T>::type >::value,
            "boost::anys::unique_any could be constructed from an rvalue of boost::any, "
            "not a lvalue."
        );
    }

    /// Moves the content of `boost::any` into *this.
    ///
    /// \throws Nothing.
    /// \post `value.empty()` is true.
    template <class BoostAny>
    unique_any(BoostAny&& value, typename std::enable_if<std::is_same<BoostAny&&, boost::any&&>::value>::type* = nullptr) noexcept
    {
        content.reset(value.content);
        value.content = nullptr;
    }

    /// Inplace constructs `T` from forwarded `args...`,
    /// so that the content of `*this` is equivalent
    /// in type to `std::decay_t<T>`.
    ///
    /// \throws std::bad_alloc or any exceptions arising from the move or
    /// copy constructor of the contained type.
    template<class T, class... Args>
    explicit unique_any(in_place_type_t<T>, Args&&... args)
      : content(new holder<typename std::decay<T>::type>(std::forward<Args>(args)...))
    {
    }

    /// Inplace constructs `T` from `li` and forwarded `args...`,
    /// so that the initial content of `*this` is equivalent
    /// in type to `std::decay_t<T>`.
    ///
    /// \throws std::bad_alloc or any exceptions arising from the move or
    /// copy constructor of the contained type.
    template <class T, class U, class... Args>
    explicit unique_any(in_place_type_t<T>, std::initializer_list<U> il, Args&&... args)
      : content(new holder<typename std::decay<T>::type>(il, std::forward<Args>(args)...))
    {
    }

    /// Releases any and all resources used in management of instance.
    ///
    /// \throws Nothing.
    ~unique_any() noexcept = default;

    /// Moves content of `rhs` into
    /// current instance, discarding previous content, so that the
    /// new content is equivalent in both type and value to the
    /// content of <code>rhs</code> before move, or empty if `rhs.empty()`.
    ///
    /// \post `rhs->empty()` is true
    /// \throws Nothing.
    unique_any & operator=(unique_any&& rhs) noexcept = default;

    /// Forwards `rhs`,
    /// discarding previous content, so that the new content of is
    /// equivalent in both type and value to `rhs` before forward.
    ///
    /// \throws std::bad_alloc
    /// or any exceptions arising from the move or copy constructor of the
    /// contained type. Assignment satisfies the strong guarantee
    /// of exception safety.
    template <class T>
    unique_any & operator=(T&& rhs)
    {
        unique_any(std::forward<T>(rhs)).swap(*this);
        return *this;
    }

    /// Inplace constructs `T` from forwarded `args...`, discarding previous
    /// content, so that the content of `*this` is equivalent
    /// in type to `std::decay_t<T>`.
    ///
    /// \returns reference to the content of `*this`.
    /// \throws std::bad_alloc or any exceptions arising from the move or
    /// copy constructor of the contained type.
    template<class T, class... Args>
    typename std::decay<T>::type& emplace(Args&&... args) {
        auto* raw_ptr = new holder<typename std::decay<T>::type>(std::forward<Args>(args)...);
        content = std::unique_ptr<boost::anys::detail::placeholder>(raw_ptr);
        return raw_ptr->held;
    }

    /// Inplace constructs `T` from `li` and forwarded `args...`, discarding
    /// previous content, so that the content of `*this` is equivalent
    /// in type to `std::decay_t<T>`.
    ///
    /// \returns reference to the content of `*this`.
    /// \throws std::bad_alloc or any exceptions arising from the move or
    /// copy constructor of the contained type.
    template<class T, class U, class... Args>
    typename std::decay<T>::type& emplace(std::initializer_list<U> il, Args&&... args) {
        auto* raw_ptr = new holder<typename std::decay<T>::type>(il, std::forward<Args>(args)...);
        content = std::unique_ptr<boost::anys::detail::placeholder>(raw_ptr);
        return raw_ptr->held;
    }

    /// \post this->has_value() is false.
    void reset() noexcept
    {
        content.reset();
    }

    /// Exchange of the contents of `*this` and `rhs`.
    ///
    /// \returns `*this`
    /// \throws Nothing.
    void swap(unique_any& rhs) noexcept
    {
        content.swap(rhs.content);
    }

    /// \returns `true` if instance is not empty, otherwise `false`.
    /// \throws Nothing.
    bool has_value() const noexcept
    {
        return !!content;
    }

    /// \returns the `typeid` of the
    /// contained value if instance is non-empty, otherwise
    /// `typeid(void)`.
    ///
    /// Useful for querying against types known either at compile time or
    /// only at runtime.
    const boost::typeindex::type_info& type() const noexcept
    {
        return content ? content->type() : boost::typeindex::type_id<void>().type_info();
    }

private: // types
    /// @cond
    template<typename T>
    class holder final: public boost::anys::detail::placeholder
    {
    public:
        template <class... Args>
        holder(Args&&... args)
          : held(std::forward<Args>(args)...)
        {
        }

        template <class U, class... Args>
        holder(std::initializer_list<U> il, Args&&... args)
          : held(il, std::forward<Args>(args)...)
        {
        }

        const boost::typeindex::type_info& type() const noexcept override
        {
            return boost::typeindex::type_id<T>().type_info();
        }

    public:
        T held;
    };

private: // representation
    template<typename T>
    friend T * unsafe_any_cast(unique_any *) noexcept;

    std::unique_ptr<boost::anys::detail::placeholder> content;
    /// @endcond
};

/// Exchange of the contents of `lhs` and `rhs`.
/// \throws Nothing.
inline void swap(unique_any & lhs, unique_any & rhs) noexcept
{
    lhs.swap(rhs);
}

/// @cond

// Note: The "unsafe" versions of any_cast are not part of the
// public interface and may be removed at any time. They are
// required where we know what type is stored in the any and can't
// use typeid() comparison, e.g., when our types may travel across
// different shared libraries.
template<typename T>
inline T * unsafe_any_cast(unique_any * operand) noexcept
{
    return std::addressof(
        static_cast<unique_any::holder<T>&>(*operand->content).held
    );
}

template<typename T>
inline const T * unsafe_any_cast(const unique_any * operand) noexcept
{
    return anys::unsafe_any_cast<T>(const_cast<unique_any *>(operand));
}
/// @endcond

/// \returns Pointer to a `T` stored in `operand`, nullptr if
/// `operand` does not contain specified `T`.
template<typename T>
T * any_cast(unique_any * operand) noexcept
{
    return operand && operand->type() == boost::typeindex::type_id<T>()
        ? anys::unsafe_any_cast<typename std::remove_cv<T>::type>(operand)
        : nullptr;
}

/// \returns Const pointer to a `T` stored in `operand`, nullptr if
/// `operand` does not contain specified `T`.
template<typename T>
inline const T * any_cast(const unique_any * operand) noexcept
{
    return anys::any_cast<T>(const_cast<unique_any *>(operand));
}

/// \returns `T` stored in `operand`
/// \throws boost::bad_any_cast if `operand` does not contain specified `T`.
template<typename T>
T any_cast(unique_any & operand)
{
    using nonref = typename std::remove_reference<T>::type;

    nonref * result = anys::any_cast<nonref>(std::addressof(operand));
    if(!result)
        boost::throw_exception(bad_any_cast());

    // Attempt to avoid construction of a temporary object in cases when
    // `T` is not a reference. Example:
    // `static_cast<std::string>(*result);`
    // which is equal to `std::string(*result);`
    typedef typename std::conditional<
        std::is_reference<T>::value,
        T,
        T&
    >::type ref_type;

#ifdef BOOST_MSVC
#   pragma warning(push)
#   pragma warning(disable: 4172) // "returning address of local variable or temporary" but *result is not local!
#endif
    return static_cast<ref_type>(*result);
#ifdef BOOST_MSVC
#   pragma warning(pop)
#endif
}

/// \returns `T` stored in `operand`
/// \throws boost::bad_any_cast if `operand` does not contain specified `T`.
template<typename T>
inline T any_cast(const unique_any & operand)
{
    using nonref = typename std::remove_reference<T>::type;
    return anys::any_cast<const nonref &>(const_cast<unique_any &>(operand));
}

/// \returns `T` stored in `operand`
/// \throws boost::bad_any_cast if `operand` does not contain specified `T`.
template<typename T>
inline T any_cast(unique_any&& operand)
{
    static_assert(
        std::is_rvalue_reference<T&&>::value /*true if T is rvalue or just a value*/
        || std::is_const< typename std::remove_reference<T>::type >::value,
        "boost::any_cast shall not be used for getting nonconst references to temporary objects"
    );
    return std::move(anys::any_cast<T&>(operand));
}

} // namespace anys

using boost::anys::any_cast;
using boost::anys::unsafe_any_cast;

} // namespace boost


#endif // BOOST_ANYS_UNIQUE_ANY_HPP_INCLUDED
