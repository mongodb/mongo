#ifndef BOOST_UUID_NIL_GENERATOR_HPP_INCLUDED
#define BOOST_UUID_NIL_GENERATOR_HPP_INCLUDED

// Copyright 2010 Andy Tompkins.
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/uuid/uuid.hpp>

namespace boost {
namespace uuids {

// generate a nil uuid
struct nil_generator
{
    using result_type = uuid;
    
    uuid operator()() const noexcept
    {
        return {{}};
    }
};

inline uuid nil_uuid() noexcept
{
    return {{}};
}

}} // namespace boost::uuids

#endif // BOOST_UUID_NIL_GENERATOR_HPP_INCLUDED
