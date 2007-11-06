///////////////////////////////////////////////////////////////////////////////
// action_state.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_ACTION_STATE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_ACTION_STATE_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <typeinfo>
#include <stdexcept>
#include <boost/assert.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // type_info_ex
    //
    template<typename T>
    struct type_info_ex
    {
        static std::type_info const *const type_info_ptr_;
    };

    template<typename T>
    std::type_info const *const type_info_ex<T>::type_info_ptr_ = &typeid(T);

    ///////////////////////////////////////////////////////////////////////////////
    // action_state
    //
    struct action_state
    {
        action_state()
          : type_ptr_(0)
          , ptr_(0)
        {
        }

        template<typename State>
        void set(State &state)
        {
            this->type_ptr_ = &type_info_ex<State>::type_info_ptr_;
            this->ptr_ = &state;
        }

        template<typename State>
        State &get() const
        {
            if(&type_info_ex<State>::type_info_ptr_ != this->type_ptr_ &&
                (0 == this->type_ptr_ || **this->type_ptr_ != typeid(State)))
            {
                throw std::invalid_argument(std::string("bad action_state_cast"));
            }

            BOOST_ASSERT(0 != this->ptr_);
            return *static_cast<State *>(this->ptr_);
        }

        std::type_info const *const *type_ptr_;
        void *ptr_;
    };

}}} // namespace boost::xpressive::detail

#endif
