/*
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   murmur3.hpp
 * \author Andrey Semashev
 * \date   16.01.2016
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * This file implements MurmurHash3 hash function implementation. See https://en.wikipedia.org/wiki/MurmurHash.
 */

#ifndef BOOST_LOG_MURMUR3_HPP_INCLUDED_
#define BOOST_LOG_MURMUR3_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! 32-bit MurmurHash3 algorithm implementation (https://en.wikipedia.org/wiki/MurmurHash)
class murmur3_32
{
private:
    uint32_t m_state;
    uint32_t m_len;

    static BOOST_CONSTEXPR_OR_CONST uint32_t c1 = 0xcc9e2d51;
    static BOOST_CONSTEXPR_OR_CONST uint32_t c2 = 0x1b873593;
    static BOOST_CONSTEXPR_OR_CONST uint32_t r1 = 15;
    static BOOST_CONSTEXPR_OR_CONST uint32_t r2 = 13;
    static BOOST_CONSTEXPR_OR_CONST uint32_t m = 5;
    static BOOST_CONSTEXPR_OR_CONST uint32_t n = 0xe6546b64;

public:
    explicit BOOST_CONSTEXPR murmur3_32(uint32_t seed) BOOST_NOEXCEPT : m_state(seed), m_len(0u)
    {
    }

    //! Mixing stage of the 32-bit MurmurHash3 algorithm
    void mix(uint32_t value) BOOST_NOEXCEPT
    {
        value *= c1;
        value = (value << r1) | (value >> (32u - r1));
        value *= c2;

        uint32_t h = m_state ^ value;
        m_state = ((h << r2) | (h >> (32u - r2))) * m + n;
        m_len += 4u;
    }

    //! Finalization stage of the 32-bit MurmurHash3 algorithm
    uint32_t finalize() BOOST_NOEXCEPT
    {
        uint32_t h = m_state ^ m_len;
        h ^= h >> 16u;
        h *= 0x85ebca6bu;
        h ^= h >> 13u;
        h *= 0xc2b2ae35u;
        h ^= h >> 16u;
        m_state = h;
        return h;
    }
};

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_MURMUR3_HPP_INCLUDED_
