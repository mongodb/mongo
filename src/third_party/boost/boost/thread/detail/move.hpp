// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-8 Anthony Williams

#ifndef BOOST_THREAD_MOVE_HPP
#define BOOST_THREAD_MOVE_HPP

#include <boost/thread/detail/config.hpp>
#ifndef BOOST_NO_SFINAE
#include <boost/utility/enable_if.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/remove_reference.hpp>
#endif

#include <boost/move/move.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{

    namespace detail
    {
        template<typename T>
        struct thread_move_t
        {
            T& t;
            explicit thread_move_t(T& t_):
                t(t_)
            {}

            T& operator*() const
            {
                return t;
            }

            T* operator->() const
            {
                return &t;
            }
        private:
            void operator=(thread_move_t&);
        };
    }

#ifndef BOOST_NO_SFINAE
    template<typename T>
    typename enable_if<boost::is_convertible<T&,boost::detail::thread_move_t<T> >, boost::detail::thread_move_t<T> >::type move(T& t)
    {
        return boost::detail::thread_move_t<T>(t);
    }
#endif

    template<typename T>
    boost::detail::thread_move_t<T> move(boost::detail::thread_move_t<T> t)
    {
        return t;
    }


}

#include <boost/config/abi_suffix.hpp>

#endif
