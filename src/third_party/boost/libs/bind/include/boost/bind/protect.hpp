#ifndef BOOST_BIND_PROTECT_HPP_INCLUDED
#define BOOST_BIND_PROTECT_HPP_INCLUDED

//
// protect.hpp
//
// Copyright 2002, 2020 Peter Dimov
// Copyright 2009 Steven Watanabe
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include <utility>

namespace boost
{

namespace _bi
{

template<class T> struct protect_make_void
{
    typedef void type;
};

template<class F, class E = void> struct protect_result_type
{
};

template<class F> struct protect_result_type< F, typename protect_make_void<typename F::result_type>::type >
{
    typedef typename F::result_type result_type;
};

template<class F> class protected_bind_t: public protect_result_type<F>
{
private:

    F f_;

public:

    explicit protected_bind_t( F f ): f_( f )
    {
    }

    template<class... A> auto operator()( A&&... a ) -> decltype( f_( std::forward<A>(a)... ) )
    {
        return f_( std::forward<A>(a)... );
    }

    template<class... A> auto operator()( A&&... a ) const -> decltype( f_( std::forward<A>(a)... ) )
    {
        return f_( std::forward<A>(a)... );
    }
};

} // namespace _bi

template<class F> _bi::protected_bind_t<F> protect(F f)
{
    return _bi::protected_bind_t<F>(f);
}

} // namespace boost

#endif // #ifndef BOOST_BIND_PROTECT_HPP_INCLUDED
