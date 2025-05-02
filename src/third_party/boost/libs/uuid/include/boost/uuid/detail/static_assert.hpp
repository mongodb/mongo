#ifndef BOOST_UUID_DETAIL_STATIC_ASSERT_INCLUDED
#define BOOST_UUID_DETAIL_STATIC_ASSERT_INCLUDED

// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#define BOOST_UUID_STATIC_ASSERT(...) static_assert(__VA_ARGS__, #__VA_ARGS__)

#endif // #ifndef BOOST_UUID_DETAIL_STATIC_ASSERT_INCLUDED
