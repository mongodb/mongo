#ifndef BOOST_SMART_PTR_DETAIL_SP_CXX20_CONSTEXPR_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_CXX20_CONSTEXPR_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

//  detail/sp_noexcept.hpp
//
//  Copyright 2025 Mathias Stearn
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt


// This macro is used to mark functions as constexpr if the compiler supports
// constexpr destructors. Since you can't have a constexpr smart pointer object,
// everything except null constructors are guided behind this macro. Because
// this also guards a use of dynamic_cast, we need to check for its availability
// as well. It isn't worth splitting out since all known compilers that support
// constexpr dynamic_cast also support constexpr destructors.
//
// WARNING: This does not check for changing active member of a union in
// constant expressions which is allowed in C++20. If that is needed, we
// need to raise the checked version to 202002L.
#if defined(__cpp_constexpr_dynamic_alloc) && __cpp_constexpr_dynamic_alloc >= 201907L \
    && defined(__cpp_constexpr) && __cpp_constexpr >= 201907L
#define BOOST_SP_CXX20_CONSTEXPR constexpr
#else
#define BOOST_SP_CXX20_CONSTEXPR
#define BOOST_SP_NO_CXX20_CONSTEXPR
#endif

#endif // #ifndef BOOST_SMART_PTR_DETAIL_SP_CXX20_CONSTEXPR_HPP_INCLUDED
