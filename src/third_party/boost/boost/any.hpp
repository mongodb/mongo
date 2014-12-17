// See http://www.boost.org/libs/any for Documentation.

#ifndef BOOST_ANY_INCLUDED
#define BOOST_ANY_INCLUDED

// what:  variant type boost::any
// who:   contributed by Kevlin Henney,
//        with features contributed and bugs found by
//        Ed Brey, Mark Rodgers, Peter Dimov, and James Curran
// when:  July 2001
// where: tested with BCC 5.5, MSVC 6.0, and g++ 2.95

#include <algorithm>
#include <typeinfo>

#include "boost/config.hpp"
#include <boost/type_traits/remove_reference.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/throw_exception.hpp>
#include <boost/static_assert.hpp>

// See boost/python/type_id.hpp
// TODO: add BOOST_TYPEID_COMPARE_BY_NAME to config.hpp
# if (defined(__GNUC__) && __GNUC__ >= 3) \
 || defined(_AIX) \
 || (   defined(__sgi) && defined(__host_mips)) \
 || (defined(__hpux) && defined(__HP_aCC)) \
 || (defined(linux) && defined(__INTEL_COMPILER) && defined(__ICC))
#  define BOOST_AUX_ANY_TYPE_ID_NAME
#include <cstring>
# endif 

namespace boost
{
    class any
    {
    public: // structors

        any()
          : content(0)
        {
        }

        template<typename ValueType>
        any(const ValueType & value)
          : content(new holder<ValueType>(value))
        {
        }

        any(const any & other)
          : content(other.content ? other.content->clone() : 0)
        {
        }

        ~any()
        {
            delete content;
        }

    public: // modifiers

        any & swap(any & rhs)
        {
            std::swap(content, rhs.content);
            return *this;
        }

        template<typename ValueType>
        any & operator=(const ValueType & rhs)
        {
            any(rhs).swap(*this);
            return *this;
        }

        any & operator=(any rhs)
        {
            rhs.swap(*this);
            return *this;
        }

    public: // queries

        bool empty() const
        {
            return !content;
        }

        const std::type_info & type() const
        {
            return content ? content->type() : typeid(void);
        }

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS
    private: // types
#else
    public: // types (public so any_cast can be non-friend)
#endif

        class placeholder
        {
        public: // structors

            virtual ~placeholder()
            {
            }

        public: // queries

            virtual const std::type_info & type() const = 0;

            virtual placeholder * clone() const = 0;

        };

        template<typename ValueType>
        class holder : public placeholder
        {
        public: // structors

            holder(const ValueType & value)
              : held(value)
            {
            }

        public: // queries

            virtual const std::type_info & type() const
            {
                return typeid(ValueType);
            }

            virtual placeholder * clone() const
            {
                return new holder(held);
            }

        public: // representation

            ValueType held;

        private: // intentionally left unimplemented
            holder & operator=(const holder &);
        };

#ifndef BOOST_NO_MEMBER_TEMPLATE_FRIENDS

    private: // representation

        template<typename ValueType>
        friend ValueType * any_cast(any *);

        template<typename ValueType>
        friend ValueType * unsafe_any_cast(any *);

#else

    public: // representation (public so any_cast can be non-friend)

#endif

        placeholder * content;

    };

    class bad_any_cast : public std::bad_cast
    {
    public:
        virtual const char * what() const throw()
        {
            return "boost::bad_any_cast: "
                   "failed conversion using boost::any_cast";
        }
    };

    template<typename ValueType>
    ValueType * any_cast(any * operand)
    {
        return operand && 
#ifdef BOOST_AUX_ANY_TYPE_ID_NAME
            std::strcmp(operand->type().name(), typeid(ValueType).name()) == 0
#else
            operand->type() == typeid(ValueType)
#endif
            ? &static_cast<any::holder<ValueType> *>(operand->content)->held
            : 0;
    }

    template<typename ValueType>
    inline const ValueType * any_cast(const any * operand)
    {
        return any_cast<ValueType>(const_cast<any *>(operand));
    }

    template<typename ValueType>
    ValueType any_cast(any & operand)
    {
        typedef BOOST_DEDUCED_TYPENAME remove_reference<ValueType>::type nonref;

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
        // If 'nonref' is still reference type, it means the user has not
        // specialized 'remove_reference'.

        // Please use BOOST_BROKEN_COMPILER_TYPE_TRAITS_SPECIALIZATION macro
        // to generate specialization of remove_reference for your class
        // See type traits library documentation for details
        BOOST_STATIC_ASSERT(!is_reference<nonref>::value);
#endif

        nonref * result = any_cast<nonref>(&operand);
        if(!result)
            boost::throw_exception(bad_any_cast());
        return *result;
    }

    template<typename ValueType>
    inline ValueType any_cast(const any & operand)
    {
        typedef BOOST_DEDUCED_TYPENAME remove_reference<ValueType>::type nonref;

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
        // The comment in the above version of 'any_cast' explains when this
        // assert is fired and what to do.
        BOOST_STATIC_ASSERT(!is_reference<nonref>::value);
#endif

        return any_cast<const nonref &>(const_cast<any &>(operand));
    }

    // Note: The "unsafe" versions of any_cast are not part of the
    // public interface and may be removed at any time. They are
    // required where we know what type is stored in the any and can't
    // use typeid() comparison, e.g., when our types may travel across
    // different shared libraries.
    template<typename ValueType>
    inline ValueType * unsafe_any_cast(any * operand)
    {
        return &static_cast<any::holder<ValueType> *>(operand->content)->held;
    }

    template<typename ValueType>
    inline const ValueType * unsafe_any_cast(const any * operand)
    {
        return unsafe_any_cast<ValueType>(const_cast<any *>(operand));
    }
}

// Copyright Kevlin Henney, 2000, 2001, 2002. All rights reserved.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#endif
