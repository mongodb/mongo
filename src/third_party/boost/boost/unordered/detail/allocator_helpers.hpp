
// Copyright 2005-2011 Daniel James.
// Copyright 2009 Pablo Halpern.
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Allocator traits written by Daniel James based on Pablo Halpern's
// implementation.

#ifndef BOOST_UNORDERED_DETAIL_ALLOCATOR_UTILITIES_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_ALLOCATOR_UTILITIES_HPP_INCLUDED

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/detail/select_type.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/enum.hpp>
#include <boost/limits.hpp>
#include <boost/type_traits/add_lvalue_reference.hpp>
#include <boost/pointer_to_other.hpp>
#include <boost/assert.hpp>
#include <boost/utility/addressof.hpp>

#if !defined(BOOST_UNORDERED_USE_ALLOCATOR_TRAITS)
#define BOOST_UNORDERED_USE_ALLOCATOR_TRAITS 0
#endif

#if BOOST_UNORDERED_USE_ALLOCATOR_TRAITS
#  include <memory>
#endif

#if !defined(BOOST_NO_0X_HDR_TYPE_TRAITS)
#  include <type_traits>
#endif

namespace boost { namespace unordered { namespace detail {

    ////////////////////////////////////////////////////////////////////////////
    // Integral_constrant, true_type, false_type
    //
    // Uses the standard versions if available.

#if !defined(BOOST_NO_0X_HDR_TYPE_TRAITS)

    using std::integral_constant;
    using std::true_type;
    using std::false_type;

#else

    template <typename T, T Value>
    struct integral_constant { enum { value = Value }; };

    typedef boost::unordered::detail::integral_constant<bool, true> true_type;
    typedef boost::unordered::detail::integral_constant<bool, false> false_type;

#endif

    ////////////////////////////////////////////////////////////////////////////
    // Explicitly call a destructor

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4100) // unreferenced formal parameter
#endif

    template <class T>
    inline void destroy(T* x) {
        x->~T();
    }

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

    ////////////////////////////////////////////////////////////////////////////
    // Bits and pieces for implementing traits
    //
    // Some of these are also used elsewhere

    template <typename T> typename boost::add_lvalue_reference<T>::type make();
    struct choice9 { typedef char (&type)[9]; };
    struct choice8 : choice9 { typedef char (&type)[8]; };
    struct choice7 : choice8 { typedef char (&type)[7]; };
    struct choice6 : choice7 { typedef char (&type)[6]; };
    struct choice5 : choice6 { typedef char (&type)[5]; };
    struct choice4 : choice5 { typedef char (&type)[4]; };
    struct choice3 : choice4 { typedef char (&type)[3]; };
    struct choice2 : choice3 { typedef char (&type)[2]; };
    struct choice1 : choice2 { typedef char (&type)[1]; };
    choice1 choose();

    typedef choice1::type yes_type;
    typedef choice2::type no_type;

    struct private_type
    {
       private_type const &operator,(int) const;
    };

    template <typename T>
    no_type is_private_type(T const&);
    yes_type is_private_type(private_type const&);

    struct convert_from_anything {
        template <typename T>
        convert_from_anything(T const&);
    };

#if !defined(BOOST_NO_SFINAE_EXPR)

#   define BOOST_UNORDERED_HAVE_CALL_DETECTION 1

    template <typename T, unsigned int> struct expr_test;
    template <typename T> struct expr_test<T, sizeof(char)> : T {};
    template <typename U> static char for_expr_test(U const&);

#define BOOST_UNORDERED_CHECK_EXPRESSION(count, result, expression)         \
        template <typename U>                                               \
        static typename boost::unordered::detail::expr_test<                \
            BOOST_PP_CAT(choice, result),                                   \
            sizeof(boost::unordered::detail::for_expr_test((                \
                (expression),                                               \
            0)))>::type test(                                               \
            BOOST_PP_CAT(choice, count))

#define BOOST_UNORDERED_DEFAULT_EXPRESSION(count, result)                   \
        template <typename U>                                               \
        static BOOST_PP_CAT(choice, result)::type test(                     \
            BOOST_PP_CAT(choice, count))

#define BOOST_UNORDERED_HAS_FUNCTION(name, thing, args, _)                  \
    struct BOOST_PP_CAT(has_, name)                                         \
    {                                                                       \
        BOOST_UNORDERED_CHECK_EXPRESSION(1, 1,                              \
            boost::unordered::detail::make< thing >().name args);           \
        BOOST_UNORDERED_DEFAULT_EXPRESSION(2, 2);                           \
                                                                            \
        enum { value = sizeof(test<T>(choose())) == sizeof(choice1::type) };\
    }

#else

#   define BOOST_UNORDERED_HAVE_CALL_DETECTION 0

    template <typename T> struct identity { typedef T type; };

#define BOOST_UNORDERED_CHECK_MEMBER(count, result, name, member)           \
                                                                            \
    typedef typename boost::unordered::detail::identity<member>::type       \
        BOOST_PP_CAT(check, count);                                         \
                                                                            \
    template <BOOST_PP_CAT(check, count) e>                                 \
    struct BOOST_PP_CAT(test, count) {                                      \
        typedef BOOST_PP_CAT(choice, result) type;                          \
    };                                                                      \
                                                                            \
    template <class U> static typename                                      \
        BOOST_PP_CAT(test, count)<&U::name>::type                           \
        test(BOOST_PP_CAT(choice, count))

#define BOOST_UNORDERED_DEFAULT_MEMBER(count, result)                       \
    template <class U> static BOOST_PP_CAT(choice, result)::type            \
        test(BOOST_PP_CAT(choice, count))

#define BOOST_UNORDERED_HAS_MEMBER(name)                                    \
    struct BOOST_PP_CAT(has_, name)                                         \
    {                                                                       \
        struct impl {                                                       \
            struct base_mixin { int name; };                                \
            struct base : public T, public base_mixin {};                   \
                                                                            \
            BOOST_UNORDERED_CHECK_MEMBER(1, 1, name, int base_mixin::*);    \
            BOOST_UNORDERED_DEFAULT_MEMBER(2, 2);                           \
                                                                            \
            enum { value = sizeof(choice2::type) ==                         \
                sizeof(test<base>(choose()))                                \
            };                                                              \
        };                                                                  \
                                                                            \
        enum { value = impl::value };                                       \
    }

#endif

    ////////////////////////////////////////////////////////////////////////////
    // Allocator traits
    //
    // Uses the standard versions if available.
    // (although untested as I don't have access to a standard version yet)

#if BOOST_UNORDERED_USE_ALLOCATOR_TRAITS

    template <typename Alloc>
    struct allocator_traits : std::allocator_traits<Alloc> {};

    template <typename Alloc, typename T>
    struct rebind_wrap
    {
        typedef typename std::allocator_traits<Alloc>::
            template rebind_alloc<T> type;
    };

#else

    // TODO: Does this match std::allocator_traits<Alloc>::rebind_alloc<T>?
    template <typename Alloc, typename T>
    struct rebind_wrap
    {
        typedef typename Alloc::BOOST_NESTED_TEMPLATE rebind<T>::other
            type;
    };

#if defined(BOOST_MSVC) && BOOST_MSVC <= 1400

    #define BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(tname)                       \
        template <typename Tp, typename Default>                            \
        struct default_type_ ## tname {                                     \
                                                                            \
            template <typename X>                                           \
            static choice1::type test(choice1, typename X::tname* = 0);     \
                                                                            \
            template <typename X>                                           \
            static choice2::type test(choice2, void* = 0);                  \
                                                                            \
            struct DefaultWrap { typedef Default tname; };                  \
                                                                            \
            enum { value = (1 == sizeof(test<Tp>(choose()))) };             \
                                                                            \
            typedef typename boost::detail::if_true<value>::                \
                BOOST_NESTED_TEMPLATE then<Tp, DefaultWrap>                 \
                ::type::tname type;                                         \
        }

#else

    template <typename T, typename T2>
    struct sfinae : T2 {};

    #define BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(tname)                       \
        template <typename Tp, typename Default>                            \
        struct default_type_ ## tname {                                     \
                                                                            \
            template <typename X>                                           \
            static typename boost::unordered::detail::sfinae<               \
                    typename X::tname, choice1>::type                       \
                test(choice1);                                              \
                                                                            \
            template <typename X>                                           \
            static choice2::type test(choice2);                             \
                                                                            \
            struct DefaultWrap { typedef Default tname; };                  \
                                                                            \
            enum { value = (1 == sizeof(test<Tp>(choose()))) };             \
                                                                            \
            typedef typename boost::detail::if_true<value>::                \
                BOOST_NESTED_TEMPLATE then<Tp, DefaultWrap>                 \
                ::type::tname type;                                         \
        }

#endif

    #define BOOST_UNORDERED_DEFAULT_TYPE(T,tname, arg)                      \
        typename default_type_ ## tname<T, arg>::type

    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(pointer);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(const_pointer);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(void_pointer);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(const_void_pointer);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(difference_type);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(size_type);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(propagate_on_container_copy_assignment);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(propagate_on_container_move_assignment);
    BOOST_UNORDERED_DEFAULT_TYPE_TMPLT(propagate_on_container_swap);

#if BOOST_UNORDERED_HAVE_CALL_DETECTION
    template <typename T>
    BOOST_UNORDERED_HAS_FUNCTION(
        select_on_container_copy_construction, U const, (), 0
    );

    template <typename T>
    BOOST_UNORDERED_HAS_FUNCTION(
        max_size, U const, (), 0
    );

    template <typename T, typename ValueType>
    BOOST_UNORDERED_HAS_FUNCTION(
        construct, U, (
            boost::unordered::detail::make<ValueType*>(),
            boost::unordered::detail::make<ValueType const>()), 2
    );

    template <typename T, typename ValueType>
    BOOST_UNORDERED_HAS_FUNCTION(
        destroy, U, (boost::unordered::detail::make<ValueType*>()), 1
    );
#else
    template <typename T>
    BOOST_UNORDERED_HAS_MEMBER(select_on_container_copy_construction);

    template <typename T>
    BOOST_UNORDERED_HAS_MEMBER(max_size);

    template <typename T, typename ValueType>
    BOOST_UNORDERED_HAS_MEMBER(construct);

    template <typename T, typename ValueType>
    BOOST_UNORDERED_HAS_MEMBER(destroy);
#endif

    template <typename Alloc>
    inline typename boost::enable_if_c<
            boost::unordered::detail::
            has_select_on_container_copy_construction<Alloc>::value, Alloc
        >::type call_select_on_container_copy_construction(const Alloc& rhs)
    {
        return rhs.select_on_container_copy_construction();
    }

    template <typename Alloc>
    inline typename boost::disable_if_c<
            boost::unordered::detail::
            has_select_on_container_copy_construction<Alloc>::value, Alloc
        >::type call_select_on_container_copy_construction(const Alloc& rhs)
    {
        return rhs;
    }

    template <typename SizeType, typename Alloc>
    inline typename boost::enable_if_c<
            boost::unordered::detail::has_max_size<Alloc>::value, SizeType
        >::type call_max_size(const Alloc& a)
    {
        return a.max_size();
    }

    template <typename SizeType, typename Alloc>
    inline typename boost::disable_if_c<
            boost::unordered::detail::has_max_size<Alloc>::value, SizeType
        >::type call_max_size(const Alloc&)
    {
        return (std::numeric_limits<SizeType>::max)();
    }

    template <typename Alloc>
    struct allocator_traits
    {
        typedef Alloc allocator_type;
        typedef typename Alloc::value_type value_type;

        typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, pointer, value_type*)
            pointer;

        template <typename T>
        struct pointer_to_other : boost::pointer_to_other<pointer, T> {};

        typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, const_pointer,
            typename pointer_to_other<const value_type>::type)
            const_pointer;

        //typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, void_pointer,
        //    typename pointer_to_other<void>::type)
        //    void_pointer;
        //
        //typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, const_void_pointer,
        //    typename pointer_to_other<const void>::type)
        //    const_void_pointer;

        typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, difference_type,
            std::ptrdiff_t) difference_type;

        typedef BOOST_UNORDERED_DEFAULT_TYPE(Alloc, size_type, std::size_t)
            size_type;

        // TODO: rebind_alloc and rebind_traits

       static pointer allocate(Alloc& a, size_type n)
            { return a.allocate(n); }

        // I never use this, so I'll just comment it out for now.
        //
        //static pointer allocate(Alloc& a, size_type n,
        //        const_void_pointer hint)
        //    { return DEFAULT_FUNC(allocate, pointer)(a, n, hint); }
    
        static void deallocate(Alloc& a, pointer p, size_type n)
            { a.deallocate(p, n); }

    public:

        // Only supporting the basic copy constructor for now.

        template <typename T>
        static typename boost::enable_if_c<
                boost::unordered::detail::has_construct<Alloc, T>::value>::type
            construct(Alloc& a, T* p, T const& x)
        {
            a.construct(p, x);
        }

        template <typename T>
        static typename boost::disable_if_c<
                boost::unordered::detail::has_construct<Alloc, T>::value>::type
            construct(Alloc&, T* p, T const& x)
        {
            new ((void*) p) T(x);
        }

        template <typename T>
        static typename boost::enable_if_c<
                boost::unordered::detail::has_destroy<Alloc, T>::value>::type
            destroy(Alloc& a, T* p)
        {
            a.destroy(p);
        }

        template <typename T>
        static typename boost::disable_if_c<
                boost::unordered::detail::has_destroy<Alloc, T>::value>::type
            destroy(Alloc&, T* p)
        {
            boost::unordered::detail::destroy(p);
        }

        static size_type max_size(const Alloc& a)
        {
            return boost::unordered::detail::call_max_size<size_type>(a);
        }

        // Allocator propagation on construction
        
        static Alloc select_on_container_copy_construction(Alloc const& rhs)
        {
            return boost::unordered::detail::
                call_select_on_container_copy_construction(rhs);
        }
    
        // Allocator propagation on assignment and swap.
        // Return true if lhs is modified.
        typedef BOOST_UNORDERED_DEFAULT_TYPE(
            Alloc, propagate_on_container_copy_assignment, false_type)
            propagate_on_container_copy_assignment;
        typedef BOOST_UNORDERED_DEFAULT_TYPE(
            Alloc,propagate_on_container_move_assignment, false_type)
            propagate_on_container_move_assignment;
        typedef BOOST_UNORDERED_DEFAULT_TYPE(
            Alloc,propagate_on_container_swap,false_type)
            propagate_on_container_swap;
    };

#undef BOOST_UNORDERED_DEFAULT_TYPE_TMPLT
#undef BOOST_UNORDERED_DEFAULT_TYPE

#endif

    // array_constructor
    //
    // Allocate and construct an array in an exception safe manner, and
    // clean up if an exception is thrown before the container takes charge
    // of it.

    template <typename Allocator>
    struct array_constructor
    {
        typedef boost::unordered::detail::allocator_traits<Allocator> traits;
        typedef typename traits::pointer pointer;

        Allocator& alloc_;
        pointer ptr_;
        pointer constructed_;
        std::size_t length_;

        array_constructor(Allocator& a)
            : alloc_(a), ptr_(), constructed_(), length_(0)
        {
            constructed_ = pointer();
            ptr_ = pointer();
        }

        ~array_constructor() {
            if (ptr_) {
                for(pointer p = ptr_; p != constructed_; ++p)
                    traits::destroy(alloc_, boost::addressof(*p));

                traits::deallocate(alloc_, ptr_, length_);
            }
        }

        template <typename V>
        void construct(V const& v, std::size_t l)
        {
            BOOST_ASSERT(!ptr_);
            length_ = l;
            ptr_ = traits::allocate(alloc_, length_);
            pointer end = ptr_ + static_cast<std::ptrdiff_t>(length_);
            for(constructed_ = ptr_; constructed_ != end; ++constructed_)
                traits::construct(alloc_, boost::addressof(*constructed_), v);
        }

        pointer get() const
        {
            return ptr_;
        }

        pointer release()
        {
            pointer p(ptr_);
            ptr_ = pointer();
            return p;
        }
    private:
        array_constructor(array_constructor const&);
        array_constructor& operator=(array_constructor const&);
    };
}}}

#endif
