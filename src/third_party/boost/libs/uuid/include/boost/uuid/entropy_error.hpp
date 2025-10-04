#ifndef BOOST_UUID_ENTROPY_ERROR_HPP_INCLUDED
#define BOOST_UUID_ENTROPY_ERROR_HPP_INCLUDED

//
// Copyright (c) 2017, 2018 James E. King III
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
//   https://www.boost.org/LICENSE_1_0.txt)
//
// Entropy error class
//

#include <boost/config.hpp>
#include <stdexcept>
#include <string>
#include <cstdint>

namespace boost {
namespace uuids {

//! \brief Given boost::system::system_error is in a module that
//!        is not header-only, we define our own exception type
//!        to handle entropy provider errors instead,
class BOOST_SYMBOL_VISIBLE entropy_error : public std::runtime_error
{
public:
    entropy_error(std::intmax_t errCode, const std::string& message)
        : std::runtime_error(message)
        , m_errcode(errCode)
    {
    }

    virtual std::intmax_t errcode() const
    {
        return m_errcode;
    }

private:
    std::intmax_t m_errcode;
};

} // uuids
} // boost

#endif // BOOST_UUID_ENTROPY_ERROR_HPP_INCLUDED
