#ifndef BOOST_BIND_PROTECT_HPP_INCLUDED
#define BOOST_BIND_PROTECT_HPP_INCLUDED

//
//  protect.hpp
//
//  Copyright (c) 2002 Peter Dimov and Multi Media Ltd.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

namespace boost
{

namespace _bi
{

template<class F> class protected_bind_t
{
public:

    typedef typename F::result_type result_type;

    explicit protected_bind_t(F f): f_(f)
    {
    }

    result_type operator()()
    {
        return f_();
    }

    result_type operator()() const
    {
        return f_();
    }

    template<class A1> result_type operator()(A1 & a1)
    {
        return f_(a1);
    }

    template<class A1> result_type operator()(A1 & a1) const
    {
        return f_(a1);
    }

    template<class A1, class A2> result_type operator()(A1 & a1, A2 & a2)
    {
        return f_(a1, a2);
    }

    template<class A1, class A2> result_type operator()(A1 & a1, A2 & a2) const
    {
        return f_(a1, a2);
    }

    template<class A1, class A2, class A3> result_type operator()(A1 & a1, A2 & a2, A3 & a3)
    {
        return f_(a1, a2, a3);
    }

    template<class A1, class A2, class A3> result_type operator()(A1 & a1, A2 & a2, A3 & a3) const
    {
        return f_(a1, a2, a3);
    }

    template<class A1, class A2, class A3, class A4> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4)
    {
        return f_(a1, a2, a3, a4);
    }

    template<class A1, class A2, class A3, class A4> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4) const
    {
        return f_(a1, a2, a3, a4);
    }

    template<class A1, class A2, class A3, class A4, class A5> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5)
    {
        return f_(a1, a2, a3, a4, a5);
    }

    template<class A1, class A2, class A3, class A4, class A5> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5) const
    {
        return f_(a1, a2, a3, a4, a5);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6)
    {
        return f_(a1, a2, a3, a4, a5, a6);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6) const
    {
        return f_(a1, a2, a3, a4, a5, a6);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7)
    {
        return f_(a1, a2, a3, a4, a5, a6, a7);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7) const
    {
        return f_(a1, a2, a3, a4, a5, a6, a7);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7, A8 & a8)
    {
        return f_(a1, a2, a3, a4, a5, a6, a7, a8);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7, A8 & a8) const
    {
        return f_(a1, a2, a3, a4, a5, a6, a7, a8);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7, A8 & a8, A9 & a9)
    {
        return f_(a1, a2, a3, a4, a5, a6, a7, a8, a9);
    }

    template<class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9> result_type operator()(A1 & a1, A2 & a2, A3 & a3, A4 & a4, A5 & a5, A6 & a6, A7 & a7, A8 & a8, A9 & a9) const
    {
        return f_(a1, a2, a3, a4, a5, a6, a7, a8, a9);
    }

private:

    F f_;
};

} // namespace _bi

template<class F> _bi::protected_bind_t<F> protect(F f)
{
    return _bi::protected_bind_t<F>(f);
}

} // namespace boost

#endif // #ifndef BOOST_BIND_PROTECT_HPP_INCLUDED
