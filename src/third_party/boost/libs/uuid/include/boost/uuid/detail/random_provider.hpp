#ifndef BOOST_UUID_DETAIL_RANDOM_PROVIDER_HPP_INCLUDED
#define BOOST_UUID_DETAIL_RANDOM_PROVIDER_HPP_INCLUDED

// Copyright (c) 2017 James E. King III
// Copyright (c) 2024 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/detail/random_device.hpp>
#include <random>
#include <cstdint>

namespace boost {
namespace uuids {
namespace detail {

class random_provider
{
private:

    detail::random_device dev_;

public:

    using result_type = std::uint32_t;

    random_provider() = default;

    // Leverage the provider as a SeedSeq for
    // PseudoRandomNumberGeneration seeding.

    template<class Iter>
    void generate(Iter first, Iter last)
    {
        std::uniform_int_distribution<std::uint32_t> dist;

        for( ; first != last; ++first )
        {
            *first = dist( dev_ );
        }
    }

    const char * name() const
    {
        return "std::random_device";
    }
};

} // detail
} // uuids
} // boost

#endif // BOOST_UUID_DETAIL_RANDOM_PROVIDER_HPP_INCLUDED
