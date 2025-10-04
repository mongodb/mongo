#ifndef BOOST_BIND_BIND_HPP_INCLUDED
#define BOOST_BIND_BIND_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
// bind.hpp - binds function objects to arguments
//
// Copyright 2001-2005, 2024 Peter Dimov
// Copyright 2001 David Abrahams
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//  See http://www.boost.org/libs/bind for documentation.
//

#include <boost/bind/mem_fn.hpp>
#include <boost/bind/arg.hpp>
#include <boost/bind/std_placeholders.hpp>
#include <boost/bind/detail/result_traits.hpp>
#include <boost/bind/detail/tuple_for_each.hpp>
#include <boost/bind/detail/integer_sequence.hpp>
#include <boost/visit_each.hpp>
#include <boost/is_placeholder.hpp>
#include <boost/type.hpp>
#include <boost/core/ref.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <utility>
#include <type_traits>
#include <tuple>

#ifdef BOOST_MSVC
# pragma warning(push)
# pragma warning(disable: 4512) // assignment operator could not be generated
#endif

namespace boost
{

template<class T> class weak_ptr;

namespace _bi // implementation details
{

// ref_compare

template<class T> bool ref_compare( T const & a, T const & b )
{
    return a == b;
}

template<int I> bool ref_compare( arg<I> const &, arg<I> const & )
{
    return true;
}

template<int I> bool ref_compare( arg<I> (*) (), arg<I> (*) () )
{
    return true;
}

template<class T> bool ref_compare( reference_wrapper<T> const & a, reference_wrapper<T> const & b )
{
    return a.get_pointer() == b.get_pointer();
}

// bind_t forward declaration for listN

template<class R, class F, class L> class bind_t;

template<class R, class F, class L> bool ref_compare( bind_t<R, F, L> const & a, bind_t<R, F, L> const & b )
{
    return a.compare( b );
}

// value

template<class T> class value
{
public:

    value(T const & t): t_(t) {}

    T & get() { return t_; }
    T const & get() const { return t_; }

    bool operator==(value const & rhs) const
    {
        return t_ == rhs.t_;
    }

private:

    T t_;
};

// ref_compare for weak_ptr

template<class T> bool ref_compare( value< weak_ptr<T> > const & a, value< weak_ptr<T> > const & b )
{
    return !(a.get() < b.get()) && !(b.get() < a.get());
}

// type

template<class T> class type {};

// unwrap

template<class F> struct unwrapper
{
    static inline F & unwrap( F & f, long )
    {
        return f;
    }

    template<class F2> static inline F2 & unwrap( reference_wrapper<F2> rf, int )
    {
        return rf.get();
    }

    template<class R, class T> static inline _mfi::dm<R, T> unwrap( R T::* pm, int )
    {
        return _mfi::dm<R, T>( pm );
    }
};

// list

template<class V> struct accept_lambda
{
    V& v_;

    explicit accept_lambda( V& v ): v_( v ) {}

    template<class A> void operator()( A& a ) const
    {
        visit_each( v_, a, 0 );
    }
};

struct equal_lambda
{
    bool result;

    equal_lambda(): result( true ) {}

    template<class A1, class A2> void operator()( A1& a1, A2& a2 )
    {
        result = result && ref_compare( a1, a2 );
    }
};

struct logical_and;
struct logical_or;

template<class... A> class list
{
private:

    typedef std::tuple<A...> data_type;
    data_type data_;

public:

    list( A... a ): data_( a... ) {}

#if defined(BOOST_MSVC)
# pragma warning( push )
# pragma warning( disable: 4100 ) // unreferenced formal parameter 'a2'
#endif

    template<class R, class F, class A2, std::size_t... I> R call_impl( type<R>, F & f, A2 & a2, _bi::index_sequence<I...> )
    {
        return unwrapper<F>::unwrap( f, 0 )( a2[ std::get<I>( data_ ) ]... );
    }

    template<class R, class F, class A2, std::size_t... I> R call_impl( type<R>, F & f, A2 & a2, _bi::index_sequence<I...> ) const
    {
        return unwrapper<F>::unwrap( f, 0 )( a2[ std::get<I>( data_ ) ]... );
    }

    template<class F, class A2, std::size_t... I> void call_impl( type<void>, F & f, A2 & a2, _bi::index_sequence<I...> )
    {
        unwrapper<F>::unwrap( f, 0 )( a2[ std::get<I>( data_ ) ]... );
    }

    template<class F, class A2, std::size_t... I> void call_impl( type<void>, F & f, A2 & a2, _bi::index_sequence<I...> ) const
    {
        unwrapper<F>::unwrap( f, 0 )( a2[ std::get<I>( data_ ) ]... );
    }

#if defined(BOOST_MSVC)
# pragma warning( pop )
#endif

    //

    template<class R, class F, class A2> R operator()( type<R>, F & f, A2 & a2 )
    {
        return call_impl( type<R>(), f, a2, _bi::index_sequence_for<A...>() );
    }

    template<class R, class F, class A2> R operator()( type<R>, F & f, A2 & a2 ) const
    {
        return call_impl( type<R>(), f, a2, _bi::index_sequence_for<A...>() );
    }

    //

    template<class A2> bool operator()( type<bool>, logical_and & /*f*/, A2 & a2 )
    {
        static_assert( sizeof...(A) == 2, "operator&& must have two arguments" );
        return a2[ std::get<0>( data_ ) ] && a2[ std::get<1>( data_ ) ];
    }

    template<class A2> bool operator()( type<bool>, logical_and const & /*f*/, A2 & a2 ) const
    {
        static_assert( sizeof...(A) == 2, "operator&& must have two arguments" );
        return a2[ std::get<0>( data_ ) ] && a2[ std::get<1>( data_ ) ];
    }

    template<class A2> bool operator()( type<bool>, logical_or & /*f*/, A2 & a2 )
    {
        static_assert( sizeof...(A) == 2, "operator|| must have two arguments" );
        return a2[ std::get<0>( data_ ) ] || a2[ std::get<1>( data_ ) ];
    }

    template<class A2> bool operator()( type<bool>, logical_or const & /*f*/, A2 & a2 ) const
    {
        static_assert( sizeof...(A) == 2, "operator|| must have two arguments" );
        return a2[ std::get<0>( data_ ) ] || a2[ std::get<1>( data_ ) ];
    }

    //

    template<class V> void accept( V & v ) const
    {
        _bi::tuple_for_each( accept_lambda<V>( v ), data_ );
    }

    bool operator==( list const & rhs ) const
    {
        return _bi::tuple_for_each( equal_lambda(), data_, rhs.data_ ).result;
    }
};

// bind_t

template<class... A> class rrlist
{
private:

    using args_type = std::tuple<A...>;

    using data_type = std::tuple<A&...>;
    data_type data_;

    template<class...> friend class rrlist;

public:

    explicit rrlist( A&... a ): data_( a... ) {}
    template<class... B> explicit rrlist( rrlist<B...> const& r ): data_( r.data_ ) {}

    template<int I> typename std::tuple_element<I-1, args_type>::type&& operator[] ( boost::arg<I> ) const
    {
        return std::forward<typename std::tuple_element<I-1, args_type>::type>( std::get<I-1>( data_ ) );
    }

    template<int I> typename std::tuple_element<I-1, args_type>::type&& operator[] ( boost::arg<I>(*)() ) const
    {
        return std::forward<typename std::tuple_element<I-1, args_type>::type>( std::get<I-1>( data_ ) );
    }

    template<class T> T & operator[] ( _bi::value<T> & v ) const { return v.get(); }

    template<class T> T const & operator[] ( _bi::value<T> const & v ) const { return v.get(); }

    template<class T> T & operator[] ( reference_wrapper<T> const & v ) const { return v.get(); }

    template<class R, class F, class L> typename result_traits<R, F>::type operator[] ( bind_t<R, F, L> & b ) const
    {
        rrlist<A&...> a2( *this );
        return b.eval( a2 );
    }

    template<class R, class F, class L> typename result_traits<R, F>::type operator[] ( bind_t<R, F, L> const & b ) const
    {
        rrlist<A&...> a2( *this );
        return b.eval( a2 );
    }
};

template<class R, class F, class L> class bind_t
{
private:

    F f_;
    L l_;

public:

    typedef typename result_traits<R, F>::type result_type;
    typedef bind_t this_type;

    bind_t( F f, L const & l ): f_( std::move(f) ), l_( l ) {}

    //

    template<class... A> result_type operator()( A&&... a )
    {
        rrlist<A...> a2( a... );
        return l_( type<result_type>(), f_, a2 );
    }

    template<class... A> result_type operator()( A&&... a ) const
    {
        rrlist<A...> a2( a... );
        return l_( type<result_type>(), f_, a2 );
    }

    //

    template<class A> result_type eval( A & a )
    {
        return l_( type<result_type>(), f_, a );
    }

    template<class A> result_type eval( A & a ) const
    {
        return l_( type<result_type>(), f_, a );
    }

    template<class V> void accept( V & v ) const
    {
        using boost::visit_each;
        visit_each( v, f_, 0 );
        l_.accept( v );
    }

    bool compare( this_type const & rhs ) const
    {
        return ref_compare( f_, rhs.f_ ) && l_ == rhs.l_;
    }
};

// function_equal

template<class R, class F, class L> bool function_equal( bind_t<R, F, L> const & a, bind_t<R, F, L> const & b )
{
    return a.compare(b);
}

// add_value

template< class T, int I > struct add_value_2
{
    typedef boost::arg<I> type;
};

template< class T > struct add_value_2< T, 0 >
{
    typedef _bi::value< T > type;
};

template<class T> struct add_value
{
    typedef typename add_value_2< T, boost::is_placeholder< T >::value >::type type;
};

template<class T> struct add_value< value<T> >
{
    typedef _bi::value<T> type;
};

template<class T> struct add_value< reference_wrapper<T> >
{
    typedef reference_wrapper<T> type;
};

template<int I> struct add_value< arg<I> >
{
    typedef boost::arg<I> type;
};

template<int I> struct add_value< arg<I> (*) () >
{
    typedef boost::arg<I> (*type) ();
};

template<class R, class F, class L> struct add_value< bind_t<R, F, L> >
{
    typedef bind_t<R, F, L> type;
};

// list_av

template<class... A> struct list_av
{
    typedef list< typename add_value<A>::type... > type;
};

// operator!

struct logical_not
{
    template<class V> bool operator()(V const & v) const { return !v; }
};

template<class R, class F, class L>
    bind_t< bool, logical_not, list< bind_t<R, F, L> > >
    operator! (bind_t<R, F, L> const & f)
{
    typedef list< bind_t<R, F, L> > list_type;
    return bind_t<bool, logical_not, list_type> ( logical_not(), list_type(f) );
}

// relational operators

#define BOOST_BIND_OPERATOR( op, name ) \
\
struct name \
{ \
    template<class V, class W> bool operator()(V const & v, W const & w) const { return v op w; } \
}; \
 \
template<class R, class F, class L, class A2> \
    bind_t< bool, name, list< bind_t<R, F, L>, typename add_value<A2>::type > > \
    operator op (bind_t<R, F, L> const & f, A2 a2) \
{ \
    typedef typename add_value<A2>::type B2; \
    typedef list< bind_t<R, F, L>, B2> list_type; \
    return bind_t<bool, name, list_type> ( name(), list_type(f, a2) ); \
}

BOOST_BIND_OPERATOR( ==, equal )
BOOST_BIND_OPERATOR( !=, not_equal )

BOOST_BIND_OPERATOR( <, less )
BOOST_BIND_OPERATOR( <=, less_equal )

BOOST_BIND_OPERATOR( >, greater )
BOOST_BIND_OPERATOR( >=, greater_equal )

BOOST_BIND_OPERATOR( &&, logical_and )
BOOST_BIND_OPERATOR( ||, logical_or )

#undef BOOST_BIND_OPERATOR

// visit_each

template<class V, class T> void visit_each( V & v, value<T> const & t, int )
{
    using boost::visit_each;
    visit_each( v, t.get(), 0 );
}

template<class V, class R, class F, class L> void visit_each( V & v, bind_t<R, F, L> const & t, int )
{
    t.accept( v );
}

} // namespace _bi

// is_bind_expression

template< class T > struct is_bind_expression
{
    enum _vt { value = 0 };
};

template< class R, class F, class L > struct is_bind_expression< _bi::bind_t< R, F, L > >
{
    enum _vt { value = 1 };
};

// bind

#ifndef BOOST_BIND
#define BOOST_BIND bind
#endif

// generic function objects

#if !BOOST_WORKAROUND(__GNUC__, < 6)

template<class R, class F, class... A>
    _bi::bind_t<R, F, typename _bi::list_av<A...>::type>
    BOOST_BIND( F f, A... a )
{
    typedef typename _bi::list_av<A...>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a... ) );
}

#else

// g++ 4.x (and some 5.x) consider boost::bind<void>( &X::f )
// ambiguous if the variadic form above is used

template<class R, class F>
    _bi::bind_t<R, F, typename _bi::list_av<>::type>
    BOOST_BIND( F f )
{
    typedef typename _bi::list_av<>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type() );
}

template<class R, class F, class A1>
    _bi::bind_t<R, F, typename _bi::list_av<A1>::type>
    BOOST_BIND( F f, A1 a1 )
{
    typedef typename _bi::list_av<A1>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1 ) );
}

template<class R, class F, class A1, class A2>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2>::type>
    BOOST_BIND( F f, A1 a1, A2 a2 )
{
    typedef typename _bi::list_av<A1, A2>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2 ) );
}

template<class R, class F, class A1, class A2, class A3>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3 )
{
    typedef typename _bi::list_av<A1, A2, A3>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4, class A5>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4, A5>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4, A5>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4, a5 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4, class A5, class A6>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4, A5, A6>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4, A5, A6>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4, a5, a6 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4, a5, a6, a7 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7, A8>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7, A8>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4, a5, a6, a7, a8 ) );
}

template<class R, class F, class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    _bi::bind_t<R, F, typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7, A8, A9>::type>
    BOOST_BIND( F f, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9 )
{
    typedef typename _bi::list_av<A1, A2, A3, A4, A5, A6, A7, A8, A9>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a1, a2, a3, a4, a5, a6, a7, a8, a9 ) );
}

#endif

// generic function objects, alternative syntax

template<class R, class F, class... A>
    _bi::bind_t<R, F, typename _bi::list_av<A...>::type>
    BOOST_BIND( boost::type<R>, F f, A... a )
{
    typedef typename _bi::list_av<A...>::type list_type;
    return _bi::bind_t<R, F, list_type>( std::move(f), list_type( a... ) );
}

// adaptable function objects

template<class F, class... A>
    _bi::bind_t<_bi::unspecified, F, typename _bi::list_av<A...>::type>
    BOOST_BIND( F f, A... a )
{
    typedef typename _bi::list_av<A...>::type list_type;
    return _bi::bind_t<_bi::unspecified, F, list_type>( std::move(f), list_type( a... ) );
}

// function pointers

#define BOOST_BIND_CC
#define BOOST_BIND_ST
#define BOOST_BIND_NOEXCEPT

#include <boost/bind/detail/bind_cc.hpp>

# if defined( __cpp_noexcept_function_type ) || defined( _NOEXCEPT_TYPES_SUPPORTED )
#   undef BOOST_BIND_NOEXCEPT
#   define BOOST_BIND_NOEXCEPT noexcept
#   include <boost/bind/detail/bind_cc.hpp>
# endif

#undef BOOST_BIND_CC
#undef BOOST_BIND_ST
#undef BOOST_BIND_NOEXCEPT

#if defined(BOOST_BIND_ENABLE_STDCALL) && !defined(_M_X64)

#define BOOST_BIND_CC __stdcall
#define BOOST_BIND_ST
#define BOOST_BIND_NOEXCEPT

#include <boost/bind/detail/bind_cc.hpp>

#undef BOOST_BIND_CC
#undef BOOST_BIND_ST
#undef BOOST_BIND_NOEXCEPT

#endif

#if defined(BOOST_BIND_ENABLE_FASTCALL) && !defined(_M_X64)

#define BOOST_BIND_CC __fastcall
#define BOOST_BIND_ST
#define BOOST_BIND_NOEXCEPT

#include <boost/bind/detail/bind_cc.hpp>

#undef BOOST_BIND_CC
#undef BOOST_BIND_ST
#undef BOOST_BIND_NOEXCEPT

#endif

#ifdef BOOST_BIND_ENABLE_PASCAL

#define BOOST_BIND_ST pascal
#define BOOST_BIND_CC
#define BOOST_BIND_NOEXCEPT

#include <boost/bind/detail/bind_cc.hpp>

#undef BOOST_BIND_ST
#undef BOOST_BIND_CC
#undef BOOST_BIND_NOEXCEPT

#endif

// member function pointers

#define BOOST_BIND_MF_NAME(X) X
#define BOOST_BIND_MF_CC
#define BOOST_BIND_MF_NOEXCEPT

#include <boost/bind/detail/bind_mf_cc.hpp>
#include <boost/bind/detail/bind_mf2_cc.hpp>

# if defined( __cpp_noexcept_function_type ) || defined( _NOEXCEPT_TYPES_SUPPORTED )
#   undef BOOST_BIND_MF_NOEXCEPT
#   define BOOST_BIND_MF_NOEXCEPT noexcept
#   include <boost/bind/detail/bind_mf_cc.hpp>
#   include <boost/bind/detail/bind_mf2_cc.hpp>
# endif

#undef BOOST_BIND_MF_NAME
#undef BOOST_BIND_MF_CC
#undef BOOST_BIND_MF_NOEXCEPT

#if defined(BOOST_MEM_FN_ENABLE_CDECL) && !defined(_M_X64)

#define BOOST_BIND_MF_NAME(X) X##_cdecl
#define BOOST_BIND_MF_CC __cdecl
#define BOOST_BIND_MF_NOEXCEPT

#include <boost/bind/detail/bind_mf_cc.hpp>
#include <boost/bind/detail/bind_mf2_cc.hpp>

#undef BOOST_BIND_MF_NAME
#undef BOOST_BIND_MF_CC
#undef BOOST_BIND_MF_NOEXCEPT

#endif

#if defined(BOOST_MEM_FN_ENABLE_STDCALL) && !defined(_M_X64)

#define BOOST_BIND_MF_NAME(X) X##_stdcall
#define BOOST_BIND_MF_CC __stdcall
#define BOOST_BIND_MF_NOEXCEPT

#include <boost/bind/detail/bind_mf_cc.hpp>
#include <boost/bind/detail/bind_mf2_cc.hpp>

#undef BOOST_BIND_MF_NAME
#undef BOOST_BIND_MF_CC
#undef BOOST_BIND_MF_NOEXCEPT

#endif

#if defined(BOOST_MEM_FN_ENABLE_FASTCALL) && !defined(_M_X64)

#define BOOST_BIND_MF_NAME(X) X##_fastcall
#define BOOST_BIND_MF_CC __fastcall
#define BOOST_BIND_MF_NOEXCEPT

#include <boost/bind/detail/bind_mf_cc.hpp>
#include <boost/bind/detail/bind_mf2_cc.hpp>

#undef BOOST_BIND_MF_NAME
#undef BOOST_BIND_MF_CC
#undef BOOST_BIND_MF_NOEXCEPT

#endif

// data member pointers

namespace _bi
{

template<class M, int I> struct add_cref;

template<class M> struct add_cref<M, 0>
{
    typedef M type;
};

template<class M> struct add_cref<M, 1>
{
    typedef M const& type;
};

template<class R> struct isref
{
    enum value_type { value = 0 };
};

template<class R> struct isref< R& >
{
    enum value_type { value = 1 };
};

template<class R> struct isref< R* >
{
    enum value_type { value = 1 };
};

template<class M, class A1, bool fn = std::is_function<M>::value> struct dm_result
{
};

template<class M, class A1> struct dm_result<M, A1, false>
{
    typedef typename add_cref< M, 1 >::type type;
};

template<class M, class R, class F, class L> struct dm_result<M, bind_t<R, F, L>, false>
{
    typedef typename bind_t<R, F, L>::result_type result_type;
    typedef typename add_cref< M, isref< result_type >::value >::type type;
};

} // namespace _bi

template<class A1, class M, class T>

_bi::bind_t<
    typename _bi::dm_result<M, A1>::type,
    _mfi::dm<M, T>,
    typename _bi::list_av<A1>::type
>

BOOST_BIND( M T::*f, A1 a1 )
{
    typedef typename _bi::dm_result<M, A1>::type result_type;
    typedef _mfi::dm<M, T> F;
    typedef typename _bi::list_av<A1>::type list_type;
    return _bi::bind_t< result_type, F, list_type >( F( f ), list_type( a1 ) );
}

} // namespace boost

#ifndef BOOST_BIND_NO_PLACEHOLDERS

# include <boost/bind/placeholders.hpp>

#endif

#ifdef BOOST_MSVC
# pragma warning(default: 4512) // assignment operator could not be generated
# pragma warning(pop)
#endif

#endif // #ifndef BOOST_BIND_BIND_HPP_INCLUDED
