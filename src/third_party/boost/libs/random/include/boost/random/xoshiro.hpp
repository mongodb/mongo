/*
 * Copyright Matt Borland 2022 - 2025.
 * Distributed under the Boost Software License, Version 1.0. (See
        * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id$
 */

#ifndef BOOST_RANDOM_XOSHIRO_HPP
#define BOOST_RANDOM_XOSHIRO_HPP

#include <boost/random/detail/config.hpp>
#include <boost/random/detail/xoshiro_base.hpp>
#include <boost/core/bit.hpp>
#include <array>
#include <cstdint>

namespace boost {
namespace random {

/**
 * This is xoshiro256++ 1.0, one of our all-purpose, rock-solid generators.
 * It has excellent (sub-ns) speed, a state (256 bits) that is large
 * enough for any parallel application, and it passes all tests we are
 * aware of.
 *
 * For generating just floating-point numbers, xoshiro256+ is even faster.
 */
class xoshiro256pp final : public detail::xoshiro_base<xoshiro256pp, 4>
{
private:

    using Base = detail::xoshiro_base<xoshiro256pp, 4>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint64_t result = boost::core::rotl(state_[0] + state_[3], 23) + state_[0];
        const std::uint64_t t = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 45);

        return result;
    }
};

/**
 * This is xoshiro256+ 1.0, our best and fastest generator for floating-point
 * numbers. We suggest to use its upper bits for floating-point
 * generation, as it is slightly faster than xoshiro256++/xoshiro256**. It
 * passes all tests we are aware of except for the lowest three bits,
 * which might fail linearity tests (and just those), so if low linear
 * complexity is not considered an issue (as it is usually the case) it
 * can be used to generate 64-bit outputs, too.
 */

class xoshiro256d final : public detail::xoshiro_base<xoshiro256d, 4, double>
{
private:

    using Base = detail::xoshiro_base<xoshiro256d, 4, double>;

public:

    using Base::Base;

    inline std::uint64_t next_int() noexcept
    {
        const std::uint64_t result = state_[0] + state_[3];
        const std::uint64_t t = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 45);

        return result;
    }

    inline result_type next() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((next_int() >> 11)) * 0x1.0p-53;
        #else
        return static_cast<double>((next_int() >> 11)) * 1.11022302462515654e-16;
        #endif
    }

    static constexpr result_type (min)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((std::numeric_limits<std::uint64_t>::min)() >> 11) * 0x1.0p-53;
        #else
        return static_cast<double>((std::numeric_limits<std::uint64_t>::min)() >> 11) * 1.11022302462515654e-16;
        #endif
    }

    static constexpr result_type (max)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((std::numeric_limits<std::uint64_t>::max)()) * 0x1.0p-53;
        #else
        return static_cast<double>((std::numeric_limits<std::uint64_t>::max)()) * 1.11022302462515654e-16;
        #endif
    }
};

/**
 * This is xoshiro256** 1.0, one of our all-purpose, rock-solid
 * generators. It has excellent (sub-ns) speed, a state (256 bits) that is
 * large enough for any parallel application, and it passes all tests we
 * are aware of.
 *
 * For generating just floating-point numbers, xoshiro256+ is even faster.
 */

class xoshiro256mm final : public detail::xoshiro_base<xoshiro256mm, 4>
{
private:

    using Base = detail::xoshiro_base<xoshiro256mm, 4>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint64_t result = boost::core::rotl(state_[1] * 5, 7) * 9U;
        const std::uint64_t t = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 45);

        return result;
    }
};

/**
 * This is xoshiro512++ 1.0, one of our all-purpose, rock-solid
 * generators. It has excellent (about 1ns) speed, a state (512 bits) that
 * is large enough for any parallel application, and it passes all tests
 * we are aware of.
 *
 * For generating just floating-point numbers, xoshiro512+ is even faster.
 *
 * The state must be seeded so that it is not everywhere zero. If you have
 * a 64-bit seed, we suggest to seed a splitmix64 generator and use its
 * output to fill s.
 */

class xoshiro512pp final : public detail::xoshiro_base<xoshiro512pp, 8>
{
private:

    using Base = detail::xoshiro_base<xoshiro512pp, 8>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint64_t result = boost::core::rotl(state_[0] + state_[2], 17) + state_[2];

        const std::uint64_t t = state_[1] << 11;

        state_[2] ^= state_[0];
        state_[5] ^= state_[1];
        state_[1] ^= state_[2];
        state_[7] ^= state_[3];
        state_[3] ^= state_[4];
        state_[4] ^= state_[5];
        state_[0] ^= state_[6];
        state_[6] ^= state_[7];

        state_[6] ^= t;

        state_[7] = boost::core::rotl(state_[7], 21);

        return result;
    }
};

/**
 * This is xoshiro512** 1.0, one of our all-purpose, rock-solid generators
 * with increased state size. It has excellent (about 1ns) speed, a state
 * (512 bits) that is large enough for any parallel application, and it
 * passes all tests we are aware of.
 *
 * For generating just floating-point numbers, xoshiro512+ is even faster.
 *
 * The state must be seeded so that it is not everywhere zero. If you have
 * a 64-bit seed, we suggest to seed a splitmix64 generator and use its
 *  output to fill s.
 */

class xoshiro512mm final : public detail::xoshiro_base<xoshiro512mm, 8>
{
private:

    using Base = detail::xoshiro_base<xoshiro512mm, 8>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint64_t result = boost::core::rotl(state_[1] * 5, 7) * 9;

        const std::uint64_t t = state_[1] << 11;

        state_[2] ^= state_[0];
        state_[5] ^= state_[1];
        state_[1] ^= state_[2];
        state_[7] ^= state_[3];
        state_[3] ^= state_[4];
        state_[4] ^= state_[5];
        state_[0] ^= state_[6];
        state_[6] ^= state_[7];

        state_[6] ^= t;

        state_[7] = boost::core::rotl(state_[7], 21);

        return result;
    }
};

/**
 * This is xoshiro512+ 1.0, our generator for floating-point numbers with
 * increased state size. We suggest to use its upper bits for
 * floating-point generation, as it is slightly faster than xoshiro512**.
 * It passes all tests we are aware of except for the lowest three bits,
 * which might fail linearity tests (and just those), so if low linear
 * complexity is not considered an issue (as it is usually the case) it
 * can be used to generate 64-bit outputs, too.
 *
 * We suggest to use a sign test to extract a random Boolean value, and
 * right shifts to extract subsets of bits.
 *
 * The state must be seeded so that it is not everywhere zero. If you have
 * a 64-bit seed, we suggest to seed a splitmix64 generator and use its
 *  output to fill s.
 */

class xoshiro512d final : public detail::xoshiro_base<xoshiro512d, 8, double>
{
private:

    using Base = detail::xoshiro_base<xoshiro512d, 8, double>;

public:

    using Base::Base;

    inline std::uint64_t next_int() noexcept
    {
        const std::uint64_t result = state_[0] + state_[2];

        const std::uint64_t t = state_[1] << 11;

        state_[2] ^= state_[0];
        state_[5] ^= state_[1];
        state_[1] ^= state_[2];
        state_[7] ^= state_[3];
        state_[3] ^= state_[4];
        state_[4] ^= state_[5];
        state_[0] ^= state_[6];
        state_[6] ^= state_[7];

        state_[6] ^= t;

        state_[7] = boost::core::rotl(state_[7], 21);

        return result;
    }

    inline result_type next() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((next_int() >> 11)) * 0x1.0p-53;
        #else
        return static_cast<double>((next_int() >> 11)) * 1.11022302462515654e-16;
        #endif
    }

    static constexpr result_type (min)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((std::numeric_limits<std::uint64_t>::min)() >> 11) * 0x1.0p-53;
        #else
        return static_cast<double>((std::numeric_limits<std::uint64_t>::min)() >> 11) * 1.11022302462515654e-16;
        #endif
    }

    static constexpr result_type (max)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<double>((std::numeric_limits<std::uint64_t>::max)() >> 11) * 0x1.0p-53;
        #else
        return static_cast<double>((std::numeric_limits<std::uint64_t>::max)() >> 11) * 1.11022302462515654e-16;
        #endif
    }
};

/**
 * This is xoshiro128++ 1.0, one of our 32-bit all-purpose, rock-solid
 * generators. It has excellent speed, a state size (128 bits) that is
 * large enough for mild parallelism, and it passes all tests we are aware
 * of.
 *
 * For generating just single-precision (i.e., 32-bit) floating-point
 * numbers, xoshiro128+ is even faster.
 *
 * The state must be seeded so that it is not everywhere zero.
 */
class xoshiro128pp final : public detail::xoshiro_base<xoshiro128pp, 4, std::uint32_t, std::uint32_t>
{
private:

    using Base = detail::xoshiro_base<xoshiro128pp, 4, std::uint32_t, std::uint32_t>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint32_t result = boost::core::rotl(state_[0] + state_[3], 7) + state_[0];

        const std::uint32_t t = state_[1] << 9;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 11);

        return result;
    }
};

/**
 * This is xoshiro128** 1.1, one of our 32-bit all-purpose, rock-solid
 * generators. It has excellent speed, a state size (128 bits) that is
 * large enough for mild parallelism, and it passes all tests we are aware
 * of.
 *
 * Note that version 1.0 had mistakenly state_[0] instead of state_[1] as state
 * word passed to the scrambler.
 *
 * For generating just single-precision (i.e., 32-bit) floating-point
 * numbers, xoshiro128+ is even faster.
 *
 * The state must be seeded so that it is not everywhere zero.
 */
class xoshiro128mm final : public detail::xoshiro_base<xoshiro128mm, 4, std::uint32_t, std::uint32_t>
{
private:

    using Base = detail::xoshiro_base<xoshiro128mm, 4, std::uint32_t, std::uint32_t>;

public:

    using Base::Base;

    inline result_type next() noexcept
    {
        const std::uint32_t result = boost::core::rotl(state_[1] * 5, 7) * 9;

        const std::uint32_t t = state_[1] << 9;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 11);

        return result;
    }
};

/**
 * This is xoshiro128+ 1.0, our best and fastest 32-bit generator for 32-bit
 * floating-point numbers. We suggest to use its upper bits for
 * floating-point generation, as it is slightly faster than xoshiro128**.
 * It passes all tests we are aware of except for
 * linearity tests, as the lowest four bits have low linear complexity, so
 * if low linear complexity is not considered an issue (as it is usually
 * the case) it can be used to generate 32-bit outputs, too.
 *
 * We suggest to use a sign test to extract a random Boolean value, and
 * right shifts to extract subsets of bits.
 *
 * The state must be seeded so that it is not everywhere zero.
 */

class xoshiro128f final : public detail::xoshiro_base<xoshiro128f, 4, float, std::uint32_t>
{
private:

    using Base = detail::xoshiro_base<xoshiro128f, 4, float, std::uint32_t>;

public:

    using Base::Base;

    inline std::uint32_t next_int() noexcept
    {
        const std::uint32_t result = state_[0] + state_[3];

        const std::uint32_t t = state_[1] << 9;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];

        state_[2] ^= t;

        state_[3] = boost::core::rotl(state_[3], 11);

        return result;
    }

    inline result_type next() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<float>((next_int() >> 8)) * 0x1.0p-24f;
        #else
        return static_cast<float>((next_int() >> 8)) * 5.9604645e-08f;
        #endif
    }

    static constexpr result_type (min)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<float>((std::numeric_limits<std::uint32_t>::min)() >> 8) * 0x1.0p-24f;
        #else
        return static_cast<float>((std::numeric_limits<std::uint64_t>::min)() >> 8) * 5.9604645e-08f;
        #endif
    }

    static constexpr result_type (max)() noexcept
    {
        #ifdef BOOST_RANDOM_HAS_HEX_FLOAT
        return static_cast<float>((std::numeric_limits<std::uint32_t>::max)() >> 8) * 0x1.0p-24f;
        #else
        return static_cast<float>((std::numeric_limits<std::uint64_t>::max)() >> 8) * 5.9604645e-08f;
        #endif
    }
};

} // namespace random
} // namespace boost

#endif //BOOST_RANDOM_XOSHIRO_HPP
