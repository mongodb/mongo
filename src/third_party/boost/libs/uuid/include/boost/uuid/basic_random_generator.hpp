#ifndef BOOST_UUID_BASIC_RANDOM_GENERATOR_HPP_INCLUDED
#define BOOST_UUID_BASIC_RANDOM_GENERATOR_HPP_INCLUDED

// Copyright 2010 Andy Tompkins
// Copyright 2017 James E. King III
// Copyright 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/detail/random_provider.hpp>
#include <boost/uuid/detail/endian.hpp>
#include <boost/assert.hpp>
#include <type_traits>
#include <random>
#include <cstdint>

namespace boost {
namespace uuids {

template<class UniformRandomNumberGenerator>
class basic_random_generator
{
private:

    UniformRandomNumberGenerator* p_;
    UniformRandomNumberGenerator g_;

public:

    using result_type = uuid;

    // default constructor creates the random number generator and
    // if the UniformRandomNumberGenerator is a PseudoRandomNumberGenerator
    // then it gets seeded by a random_provider.
    basic_random_generator(): p_( 0 ), g_()
    {
        // seed the random number generator if it is capable
        seed( g_, 0 );
    }

    // keep a reference to a random number generator
    // don't seed a given random number generator
    explicit basic_random_generator( UniformRandomNumberGenerator& gen ): p_( &gen )
    {
    }

    // keep a pointer to a random number generator
    // don't seed a given random number generator
    explicit basic_random_generator( UniformRandomNumberGenerator* gen ): p_( gen )
    {
        BOOST_ASSERT( gen != 0 );
    }

    result_type operator()()
    {
        UniformRandomNumberGenerator& gen = p_? *p_: g_;

        std::uniform_int_distribution<std::uint32_t> dist;

        result_type u;

        detail::store_native_u32( u.data +  0, dist( gen ) );
        detail::store_native_u32( u.data +  4, dist( gen ) );
        detail::store_native_u32( u.data +  8, dist( gen ) );
        detail::store_native_u32( u.data + 12, dist( gen ) );

        // set variant
        // must be 0b10xxxxxx
        *(u.begin() + 8) &= 0x3F;
        *(u.begin() + 8) |= 0x80;

        // set version
        // must be 0b0100xxxx
        *(u.begin() + 6) &= 0x0F; //0b00001111
        *(u.begin() + 6) |= 0x40; //0b01000000

        return u;
    }

private:

    // Detect whether UniformRandomNumberGenerator has a seed() method which indicates that
    // it is a PseudoRandomNumberGenerator and needs a seed to initialize it.  This allows
    // basic_random_generator to take any type of UniformRandomNumberGenerator and still
    // meet the post-conditions for the default constructor.

    template<class MaybePseudoRandomNumberGenerator, class En = decltype( std::declval<MaybePseudoRandomNumberGenerator&>().seed() )>
    void seed( MaybePseudoRandomNumberGenerator& rng, int )
    {
        detail::random_provider seeder;
        rng.seed(seeder);
    }

    template<class MaybePseudoRandomNumberGenerator>
    void seed( MaybePseudoRandomNumberGenerator&, long )
    {
    }
};

}} // namespace boost::uuids

#endif // BOOST_UUID_BASIC_RANDOM_GENERATOR_HPP_INCLUDED
