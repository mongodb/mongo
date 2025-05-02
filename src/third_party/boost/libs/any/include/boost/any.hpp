// See http://www.boost.org/libs/any for Documentation.

#ifndef BOOST_ANY_INCLUDED
#define BOOST_ANY_INCLUDED

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
# pragma once
#endif

/// \file boost/any.hpp
/// \brief \copybrief boost::any

// what:  variant type boost::any
// who:   contributed by Kevlin Henney,
//        with features contributed and bugs found by
//        Antony Polukhin, Ed Brey, Mark Rodgers,
//        Peter Dimov, and James Curran
// when:  July 2001, April 2013 - 2020

#include <boost/any/bad_any_cast.hpp>
#include <boost/any/fwd.hpp>
#include <boost/any/detail/placeholder.hpp>
#include <boost/throw_exception.hpp>
#include <boost/type_index.hpp>

#include <memory>  // for std::addressof
#include <type_traits>

namespace boost
{
    /// \brief A class whose instances can hold instances of any
    /// type that satisfies \forcedlink{ValueType} requirements.
    class any
    {
    public:

        /// \post this->empty() is true.
        constexpr any() noexcept
          : content(0)
        {
        }

        /// Makes a copy of `value`, so
        /// that the initial content of the new instance is equivalent
        /// in both type and value to `value`.
        ///
        /// \throws std::bad_alloc or any exceptions arising from the copy
        /// constructor of the contained type.
        template<typename ValueType>
        any(const ValueType & value)
          : content(new holder<
                typename std::remove_cv<typename std::decay<const ValueType>::type>::type
            >(value))
        {
            static_assert(
                !anys::detail::is_basic_any<ValueType>::value,
                "boost::any shall not be constructed from boost::anys::basic_any"
            );
        }

        /// Copy constructor that copies content of
        /// `other` into new instance, so that any content
        /// is equivalent in both type and value to the content of
        /// `other`, or empty if `other` is empty.
        ///
        /// \throws May fail with a `std::bad_alloc`
        /// exception or any exceptions arising from the copy
        /// constructor of the contained type.
        any(const any & other)
          : content(other.content ? other.content->clone() : 0)
        {
        }

        /// Move constructor that moves content of
        /// `other` into new instance and leaves `other` empty.
        ///
        /// \post other->empty() is true
        /// \throws Nothing.
        any(any&& other) noexcept
          : content(other.content)
        {
            other.content = 0;
        }

        /// Forwards `value`, so
        /// that the initial content of the new instance is equivalent
        /// in both type and value to `value` before the forward.
        ///
        /// \throws std::bad_alloc or any exceptions arising from the move or
        /// copy constructor of the contained type.
        template<typename ValueType>
        any(ValueType&& value
            , typename std::enable_if<!std::is_same<any&, ValueType>::value >::type* = 0 // disable if value has type `any&`
            , typename std::enable_if<!std::is_const<ValueType>::value >::type* = 0) // disable if value has type `const ValueType&&`
          : content(new holder< typename std::decay<ValueType>::type >(std::forward<ValueType>(value)))
        {
            static_assert(
                !anys::detail::is_basic_any<typename std::decay<ValueType>::type>::value,
                "boost::any shall not be constructed from boost::anys::basic_any"
            );
        }

        /// Releases any and all resources used in management of instance.
        ///
        /// \throws Nothing.
        ~any() noexcept
        {
            delete content;
        }

    public: // modifiers

        /// Exchange of the contents of `*this` and `rhs`.
        ///
        /// \returns `*this`
        /// \throws Nothing.
        any & swap(any & rhs) noexcept
        {
            placeholder* tmp = content;
            content = rhs.content;
            rhs.content = tmp;
            return *this;
        }

        /// Copies content of `rhs` into
        /// current instance, discarding previous content, so that the
        /// new content is equivalent in both type and value to the
        /// content of `rhs`, or empty if `rhs.empty()`.
        ///
        /// \throws std::bad_alloc
        /// or any exceptions arising from the copy constructor of the
        /// contained type. Assignment satisfies the strong guarantee
        /// of exception safety.
        any & operator=(const any& rhs)
        {
            any(rhs).swap(*this);
            return *this;
        }

        /// Moves content of `rhs` into
        /// current instance, discarding previous content, so that the
        /// new content is equivalent in both type and value to the
        /// content of `rhs` before move, or empty if
        /// `rhs.empty()`.
        ///
        /// \post `rhs->empty()` is true
        /// \throws Nothing.
        any & operator=(any&& rhs) noexcept
        {
            rhs.swap(*this);
            any().swap(rhs);
            return *this;
        }

        /// Forwards `rhs`,
        /// discarding previous content, so that the new content of is
        /// equivalent in both type and value to
        /// `rhs` before forward.
        ///
        /// \throws std::bad_alloc
        /// or any exceptions arising from the move or copy constructor of the
        /// contained type. Assignment satisfies the strong guarantee
        /// of exception safety.
        template <class ValueType>
        any & operator=(ValueType&& rhs)
        {
            static_assert(
                !anys::detail::is_basic_any<typename std::decay<ValueType>::type>::value,
                "boost::anys::basic_any shall not be assigned into boost::any"
            );
            any(std::forward<ValueType>(rhs)).swap(*this);
            return *this;
        }

    public: // queries

        /// \returns `true` if instance is empty, otherwise `false`.
        /// \throws Nothing.
        bool empty() const noexcept
        {
            return !content;
        }

        /// \post this->empty() is true
        void clear() noexcept
        {
            any().swap(*this);
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
        class BOOST_SYMBOL_VISIBLE placeholder: public boost::anys::detail::placeholder
        {
        public:
            virtual placeholder * clone() const = 0;
        };

        template<typename ValueType>
        class holder final
          : public placeholder
        {
        public: // structors

            holder(const ValueType & value)
              : held(value)
            {
            }

            holder(ValueType&& value)
              : held(static_cast< ValueType&& >(value))
            {
            }

        public: // queries

            const boost::typeindex::type_info& type() const noexcept override
            {
                return boost::typeindex::type_id<ValueType>().type_info();
            }

            placeholder * clone() const BOOST_OVERRIDE
            {
                return new holder(held);
            }

        public: // representation

            ValueType held;

        private: // intentionally left unimplemented
            holder & operator=(const holder &);
        };

    private: // representation
        template<typename ValueType>
        friend ValueType * unsafe_any_cast(any *) noexcept;

        friend class boost::anys::unique_any;

        placeholder * content;
        /// @endcond
    };

    /// Exchange of the contents of `lhs` and `rhs`.
    /// \throws Nothing.
    inline void swap(any & lhs, any & rhs) noexcept
    {
        lhs.swap(rhs);
    }

    /// @cond

    // Note: The "unsafe" versions of any_cast are not part of the
    // public interface and may be removed at any time. They are
    // required where we know what type is stored in the any and can't
    // use typeid() comparison, e.g., when our types may travel across
    // different shared libraries.
    template<typename ValueType>
    inline ValueType * unsafe_any_cast(any * operand) noexcept
    {
        return std::addressof(
            static_cast<any::holder<ValueType> *>(operand->content)->held
        );
    }

    template<typename ValueType>
    inline const ValueType * unsafe_any_cast(const any * operand) noexcept
    {
        return boost::unsafe_any_cast<ValueType>(const_cast<any *>(operand));
    }
    /// @endcond

    /// \returns Pointer to a ValueType stored in `operand`, nullptr if
    /// `operand` does not contain specified `ValueType`.
    template<typename ValueType>
    ValueType * any_cast(any * operand) noexcept
    {
        return operand && operand->type() == boost::typeindex::type_id<ValueType>()
            ? boost::unsafe_any_cast<typename std::remove_cv<ValueType>::type>(operand)
            : 0;
    }

    /// \returns Const pointer to a ValueType stored in `operand`, nullptr if
    /// `operand` does not contain specified `ValueType`.
    template<typename ValueType>
    inline const ValueType * any_cast(const any * operand) noexcept
    {
        return boost::any_cast<ValueType>(const_cast<any *>(operand));
    }

    /// \returns ValueType stored in `operand`
    /// \throws boost::bad_any_cast if `operand` does not contain 
    /// specified ValueType.
    template<typename ValueType>
    ValueType any_cast(any & operand)
    {
        using nonref = typename std::remove_reference<ValueType>::type;

        nonref * result = boost::any_cast<nonref>(std::addressof(operand));
        if(!result)
            boost::throw_exception(bad_any_cast());

        // Attempt to avoid construction of a temporary object in cases when
        // `ValueType` is not a reference. Example:
        // `static_cast<std::string>(*result);`
        // which is equal to `std::string(*result);`
        typedef typename std::conditional<
            std::is_reference<ValueType>::value,
            ValueType,
            typename std::add_lvalue_reference<ValueType>::type
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

    /// \returns `ValueType` stored in `operand`
    /// \throws boost::bad_any_cast if `operand` does not contain 
    /// specified `ValueType`.
    template<typename ValueType>
    inline ValueType any_cast(const any & operand)
    {
        using nonref = typename std::remove_reference<ValueType>::type;
        return boost::any_cast<const nonref &>(const_cast<any &>(operand));
    }

    /// \returns `ValueType` stored in `operand`, leaving the `operand` empty.
    /// \throws boost::bad_any_cast if `operand` does not contain 
    /// specified `ValueType`.
    template<typename ValueType>
    inline ValueType any_cast(any&& operand)
    {
        static_assert(
            std::is_rvalue_reference<ValueType&&>::value /*true if ValueType is rvalue or just a value*/
            || std::is_const< typename std::remove_reference<ValueType>::type >::value,
            "boost::any_cast shall not be used for getting nonconst references to temporary objects"
        );
        return boost::any_cast<ValueType>(operand);
    }
}

// Copyright Kevlin Henney, 2000, 2001, 2002. All rights reserved.
// Copyright Antony Polukhin, 2013-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#endif
