#ifndef BOOST_UUID_RANDOM_GENERATOR_HPP_INCLUDED
#define BOOST_UUID_RANDOM_GENERATOR_HPP_INCLUDED

// Copyright 2010 Andy Tompkins.
// Copyright 2017 James E. King III
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/basic_random_generator.hpp>
#include <boost/uuid/detail/random_device.hpp>
#include <boost/uuid/detail/chacha20.hpp>

namespace boost {
namespace uuids {

// the default random generator
class random_generator: public basic_random_generator<detail::chacha20_12>
{
};

// only provided for compatibility with 1.85
using random_generator_mt19937 = basic_random_generator<std::mt19937>;

// only provided for compatibility with 1.85
using random_generator_pure = basic_random_generator<detail::random_device>;

}} // namespace boost::uuids

#endif // BOOST_UUID_RANDOM_GENERATOR_HPP_INCLUDED
