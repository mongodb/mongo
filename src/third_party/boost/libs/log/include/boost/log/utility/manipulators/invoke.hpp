/*
 *             Copyright Andrey Semashev 2022.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          https://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   utility/manipulators/invoke.hpp
 * \author Andrey Semashev
 * \date   27.02.2022
 *
 * The header contains implementation of a stream manipulator for invoking a user-defined function.
 */

#ifndef BOOST_LOG_UTILITY_MANIPULATORS_INVOKE_HPP_INCLUDED_
#define BOOST_LOG_UTILITY_MANIPULATORS_INVOKE_HPP_INCLUDED_

#include <cstddef>
#include <boost/core/enable_if.hpp>
#include <boost/log/detail/config.hpp>
#include <boost/type_traits/remove_cv.hpp>
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#include <boost/type_traits/remove_reference.hpp>
#endif
#include <boost/log/detail/is_ostream.hpp>
#include <boost/log/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

/*!
 * Stream manipulator for invoking a user-defined function as part of stream output.
 */
template< typename FunctionT >
class invoke_manipulator
{
private:
    mutable FunctionT m_function;

public:
    //! Initializing constructor
    explicit invoke_manipulator(FunctionT const& func) :
        m_function(func)
    {
    }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    //! Initializing constructor
    explicit invoke_manipulator(FunctionT&& func) :
        m_function(static_cast< FunctionT&& >(func))
    {
    }
#endif

    //! The method invokes the saved function with the output stream
    template< typename StreamT >
    void output(StreamT& stream) const
    {
        m_function(stream);
    }
};

/*!
 * Stream output operator for \c invoke_manipulator. Invokes the function saved in the manipulator.
 */
template< typename StreamT, typename FunctionT >
inline typename boost::enable_if_c< log::aux::is_ostream< StreamT >::value, StreamT& >::type operator<< (StreamT& stream, invoke_manipulator< FunctionT > const& manip)
{
    manip.output(stream);
    return stream;
}

#if !defined(BOOST_LOG_DOXYGEN_PASS)

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)

//! Invoke manipulator generator function
template< typename FunctionT >
inline invoke_manipulator<
    typename boost::remove_cv<
        typename boost::remove_reference< FunctionT >::type
    >::type
>
invoke_manip(FunctionT&& func)
{
    return invoke_manipulator<
        typename boost::remove_cv<
            typename boost::remove_reference< FunctionT >::type
        >::type
    >(static_cast< FunctionT&& >(func));
}

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) && \
    !defined(BOOST_NO_CXX14_GENERIC_LAMBDAS) && \
    !defined(BOOST_NO_CXX14_RETURN_TYPE_DEDUCTION)

//! Invoke manipulator generator function
template< typename FunctionT, typename Arg0, typename... Args >
inline auto invoke_manip(FunctionT&& func, Arg0&& arg0, Args&&... args)
{
    return boost::log::invoke_manip
    (
#if !defined(BOOST_LOG_NO_CXX20_PACK_EXPANSION_IN_LAMBDA_INIT_CAPTURE)
        [func = static_cast< FunctionT&& >(func), arg0 = static_cast< Arg0&& >(arg0), ...args = static_cast< Args&& >(args)](auto& stream) mutable
#else
        [func, arg0, args...](auto& stream) mutable
#endif
        {
#if !defined(BOOST_MSVC) || BOOST_MSVC >= 1910
            static_cast< FunctionT&& >(func)(stream, static_cast< Arg0&& >(arg0), static_cast< Args&& >(args)...);
#else
            // MSVC 19.0 (VS 14.0) ICEs if we use perfect forwarding here
            func(stream, arg0, args...);
#endif
        }
    );
}

#endif // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) ...

#else // !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)

//! Invoke manipulator generator function
template< typename FunctionT >
inline invoke_manipulator< typename boost::remove_cv< FunctionT >::type >
invoke_manip(FunctionT const& func)
{
    return invoke_manipulator< typename boost::remove_cv< FunctionT >::type >(func);
}

#endif // !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)

#else // !defined(BOOST_LOG_DOXYGEN_PASS)

/*!
 * Invoke manipulator generator function.
 *
 * \param func User-defined function to invoke on output. The function must be callable with a reference to the output stream as the first argument, followed by \a args.
 * \param args Additional arguments to pass to \a func.
 * \returns Manipulator to be inserted into the stream.
 *
 * \note \a args are only supported since C++14.
 */
template< typename FunctionT, typename... Args >
invoke_manipulator< unspecified > invoke_manip(FunctionT&& func, Args&&... args);

#endif // !defined(BOOST_LOG_DOXYGEN_PASS)

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_UTILITY_MANIPULATORS_INVOKE_HPP_INCLUDED_
