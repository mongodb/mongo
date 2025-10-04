// Copyright (C) 2004 Arkadiy Vertleyb
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_TYPEOF_TYPEOF_HPP_INCLUDED
#define BOOST_TYPEOF_TYPEOF_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/config/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1900)

# include <boost/typeof/msvc/typeof_impl.hpp>

# define BOOST_TYPEOF_REGISTER_TYPE(x)
# define BOOST_TYPEOF_REGISTER_TEMPLATE(x, params)

#else

# include <boost/typeof/decltype.hpp>

#endif

#define BOOST_TYPEOF_UNIQUE_ID()\
     BOOST_TYPEOF_REGISTRATION_GROUP * 0x10000 + __LINE__

#define BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()\
     <boost/typeof/incr_registration_group.hpp>

// auto
#define BOOST_AUTO(Var, Expr) auto Var = Expr
#define BOOST_AUTO_TPL(Var, Expr) auto Var = Expr

#endif//BOOST_TYPEOF_TYPEOF_HPP_INCLUDED
