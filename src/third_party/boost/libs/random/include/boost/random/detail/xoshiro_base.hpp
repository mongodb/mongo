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

#ifndef BOOST_RANDOM_DETAIL_XOSHIRO_BASE
#define BOOST_RANDOM_DETAIL_XOSHIRO_BASE

#include <boost/random/splitmix64.hpp>
#include <boost/random/detail/seed.hpp>
#include <boost/throw_exception.hpp>
#include <boost/config.hpp>
#include <array>
#include <utility>
#include <stdexcept>
#include <limits>
#include <initializer_list>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <ios>
#include <istream>
#include <type_traits>
#include <iterator>

namespace boost {
namespace random {
namespace detail {

// N is the number of words (e.g. for xoshiro 256 N=4)
template <typename Derived, std::size_t N, typename OutputType = std::uint64_t, typename BlockType = std::uint64_t>
class xoshiro_base
{
protected:

    std::array<BlockType, N> state_;

private:

    using xoshiro_type = std::integral_constant<BlockType, N>;

    inline std::uint64_t concatenate(std::uint32_t word1, std::uint32_t word2) noexcept
    {
        return static_cast<std::uint64_t>(word1) << 32U | word2;
    }

    template <typename Sseq>
    inline void sseq_seed_64(Sseq& seq)
    {
        for (auto& i : state_)
        {
            std::array<std::uint32_t, 2> seeds;
            seq.generate(seeds.begin(), seeds.end());

            i = concatenate(seeds[0], seeds[1]);
        }
    }

    template <typename Sseq>
    inline void sseq_seed_32(Sseq& seq)
    {
        seq.generate(state_.begin(), state_.end());
    }

    inline void jump_impl(const std::integral_constant<std::uint64_t, 4>&) noexcept
    {
        constexpr std::array<std::uint64_t, 4U> jump = {{ UINT64_C(0x180ec6d33cfd0aba), UINT64_C(0xd5a61266f0c9392c),
                                                          UINT64_C(0xa9582618e03fc9aa), UINT64_C(0x39abdc4529b1661c) }};

        std::uint64_t s0 = 0;
        std::uint64_t s1 = 0;
        std::uint64_t s2 = 0;
        std::uint64_t s3 = 0;

        for (std::uint64_t i = 0; i < jump.size(); i++)
        {
            for (std::size_t b = 0; b < 64U; b++)
            {
                if (jump[i] & UINT64_C(1) << b) {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }

                next();
            }
        }

        state_[0] = s0;
        state_[1] = s1;
        state_[2] = s2;
        state_[3] = s3;
    }

    inline void jump_impl(const std::integral_constant<std::uint64_t, 8>&) noexcept
    {
        constexpr std::array<std::uint64_t, 8U> jump = {{ UINT64_C(0x33ed89b6e7a353f9), UINT64_C(0x760083d7955323be),
                                                          UINT64_C(0x2837f2fbb5f22fae), UINT64_C(0x4b8c5674d309511c),
                                                          UINT64_C(0xb11ac47a7ba28c25), UINT64_C(0xf1be7667092bcc1c),
                                                          UINT64_C(0x53851efdb6df0aaf), UINT64_C(0x1ebbc8b23eaf25db) }};

        std::array<std::uint64_t, 8U> t = {{ 0, 0, 0, 0, 0, 0, 0, 0 }};

        for (std::size_t i = 0; i < jump.size(); ++i)
        {
            for (std::size_t b = 0; b < 64U; ++b)
            {
                if (jump[i] & UINT64_C(1) << b)
                {
                    for (std::size_t w = 0; w < state_.size(); ++w)
                    {
                        t[w] ^= state_[w];
                    }
                }

                next();
            }
        }

        state_ = t;
    }

    inline void long_jump_impl(const std::integral_constant<std::uint64_t, 4>&) noexcept
    {
        constexpr std::array<std::uint64_t, 4> long_jump = {{ UINT64_C(0x76e15d3efefdcbbf), UINT64_C(0xc5004e441c522fb3),
                                                              UINT64_C(0x77710069854ee241), UINT64_C(0x39109bb02acbe635) }};

        std::uint64_t s0 = 0;
        std::uint64_t s1 = 0;
        std::uint64_t s2 = 0;
        std::uint64_t s3 = 0;

        for (std::size_t i = 0; i < long_jump.size(); i++)
        {
            for (std::size_t b = 0; b < 64; b++)
            {
                if (long_jump[i] & UINT64_C(1) << b)
                {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }

                next();
            }
        }

        state_[0] = s0;
        state_[1] = s1;
        state_[2] = s2;
        state_[3] = s3;
    }

    inline void long_jump_impl(const std::integral_constant<std::uint64_t, 8>&) noexcept
    {
        constexpr std::array<std::uint64_t, 8U> long_jump = {{ UINT64_C(0x11467fef8f921d28), UINT64_C(0xa2a819f2e79c8ea8),
                                                               UINT64_C(0xa8299fc284b3959a), UINT64_C(0xb4d347340ca63ee1),
                                                               UINT64_C(0x1cb0940bedbff6ce), UINT64_C(0xd956c5c4fa1f8e17),
                                                               UINT64_C(0x915e38fd4eda93bc), UINT64_C(0x5b3ccdfa5d7daca5) }};

        std::array<std::uint64_t, 8U> t = {{ 0, 0, 0, 0, 0, 0, 0, 0 }};

        for (std::size_t i = 0; i < long_jump.size(); ++i)
        {
            for (std::size_t b = 0; b < 64U; ++b)
            {
                if (long_jump[i] & UINT64_C(1) << b)
                {
                    for (std::size_t w = 0; w < state_.size(); ++w)
                    {
                        t[w] ^= state_[w];
                    }
                }

                next();
            }
        }

        state_ = t;
    }

    inline void jump_impl(const std::integral_constant<std::uint32_t, 4>&) noexcept
    {
        constexpr std::array<std::uint32_t, 4> jump = {{ UINT32_C(0x8764000b), UINT32_C(0xf542d2d3), 
                                                         UINT32_C(0x6fa035c3), UINT32_C(0x77f2db5b) }};

        std::uint32_t s0 = 0;
        std::uint32_t s1 = 0;
        std::uint32_t s2 = 0;
        std::uint32_t s3 = 0;

        for (std::size_t i = 0; i < jump.size(); i++)
        {
            for (std::size_t b = 0; b < 32U; b++)
            {
                if (jump[i] & UINT32_C(1) << b)
                {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }

                next();
            }
        }

        state_[0] = s0;
        state_[1] = s1;
        state_[2] = s2;
        state_[3] = s3;
    }

    inline void long_jump_impl(const std::integral_constant<std::uint32_t, 4>&) noexcept
    {
        constexpr std::array<std::uint32_t, 4> jump = {{ UINT32_C(0xb523952e), UINT32_C(0x0b6f099f),
                                                         UINT32_C(0xccf5a0ef), UINT32_C(0x1c580662) }};

        std::uint32_t s0 = 0;
        std::uint32_t s1 = 0;
        std::uint32_t s2 = 0;
        std::uint32_t s3 = 0;

        for (std::size_t i = 0; i < jump.size(); i++)
        {
            for (std::size_t b = 0; b < 32; b++)
            {
                if (jump[i] & UINT32_C(1) << b)
                {
                    s0 ^= state_[0];
                    s1 ^= state_[1];
                    s2 ^= state_[2];
                    s3 ^= state_[3];
                }

                next();
            }
        }

        state_[0] = s0;
        state_[1] = s1;
        state_[2] = s2;
        state_[3] = s3;
    }

public:

    using result_type = OutputType;
    using seed_type = BlockType;

    static constexpr bool has_fixed_range {false};

    /** Seeds the generator using the default seed of boost::random::splitmix64 */
    void seed()
    {
        splitmix64 gen;
        for (auto& i : state_)
        {
            i = static_cast<seed_type>(gen());
        }
    }

    /** Seeds the generator with a user provided seed. */
    void seed(const seed_type value)
    {
        splitmix64 gen(value);
        for (auto& i : state_)
        {
            i = static_cast<seed_type>(gen());
        }
    }

    /**
     * Seeds the generator with 32-bit values produced by @c seq.generate().
     */
    template <typename Sseq, typename std::enable_if<!std::is_convertible<Sseq, seed_type>::value, bool>::type = true>
    void seed(Sseq& seq)
    {
        BOOST_IF_CONSTEXPR (std::is_same<BlockType, std::uint64_t>::value)
        {
            sseq_seed_64(seq);
        }
        else
        {
            sseq_seed_32(seq);
        }
    }

    /** Sets the state of the generator using values from an iterator range. */
    template <typename FIter>
    void seed(FIter first, FIter last)
    {
        static_assert(std::is_integral<typename std::iterator_traits<FIter>::value_type>::value,
                      "Value type must be a built-in integer type" );

        std::size_t offset = 0;
        while (first != last && offset < state_.size())
        {
            state_[offset++] = static_cast<seed_type>(*first++);
        }

        if (offset != state_.size())
        {
            boost::throw_exception(std::invalid_argument("Not enough elements in call to seed."));
        }
    }

    /**
     * Constructs a @c xoshiro and calls @c seed().
     */
    xoshiro_base() { seed(); }

    /** Seeds the generator with a user provided seed. */
    explicit xoshiro_base(const seed_type value)
    {
        seed(value);
    }

    template <typename FIter>
    xoshiro_base(FIter& first, FIter last) { seed(first, last); }

    /**
     * Seeds the generator with 64-bit values produced by @c seq.generate().
     *
     * @xmlnote
     * The copy constructor will always be preferred over
     * the templated constructor.
     * @endxmlnote
     */
    template <typename Sseq, typename std::enable_if<!std::is_convertible<Sseq, xoshiro_base>::value, bool>::type = true>
    explicit xoshiro_base(Sseq& seq)
    {
        seed(seq);
    }

    // Hit all of our rule of 5 explicitly to ensure old platforms work correctly
    ~xoshiro_base() = default;
    xoshiro_base(const xoshiro_base& other) noexcept { state_ = other.state(); }
    xoshiro_base& operator=(const xoshiro_base& other) noexcept { state_ = other.state(); return *this; }
    xoshiro_base(xoshiro_base&& other) noexcept { state_ = other.state(); }
    xoshiro_base& operator=(xoshiro_base&& other) noexcept { state_ = other.state(); return *this; }

    inline result_type next() noexcept
    {
        return static_cast<Derived*>(this)->next();
    }

    /** This is the jump function for the generator. It is equivalent
     *  to 2^128 calls to next(); it can be used to generate 2^128
     *  non-overlapping subsequences for parallel computations. */
    inline void jump() noexcept
    {
        jump_impl(xoshiro_type());
    }

    /** This is the long-jump function for the generator. It is equivalent to
     *  2^192 calls to next(); it can be used to generate 2^64 starting points,
     *  from each of which jump() will generate 2^64 non-overlapping
     *  subsequences for parallel distributed computations. */
    inline void long_jump() noexcept
    {
        long_jump_impl(xoshiro_type());
    }

    /**  Returns the next value of the generator. */
    inline result_type operator()() noexcept
    {
        return next();
    }

    /** Advances the state of the generator by @c z. */
    inline void discard(const std::uint64_t z) noexcept
    {
        for (std::uint64_t i {}; i < z; ++i)
        {
            next();
        }
    }

    /**
     * Returns true if the two generators will produce identical
     * sequences of values.
     */
    inline friend bool operator==(const xoshiro_base& lhs, const xoshiro_base& rhs) noexcept
    {
        return lhs.state_ == rhs.state_;
    }

    /**
     * Returns true if the two generators will produce different
     * sequences of values.
     */
    inline friend bool operator!=(const xoshiro_base& lhs, const xoshiro_base& rhs) noexcept
    {
        return lhs.state_ != rhs.state_;
    }

    /**  Writes a @c xorshiro to a @c std::ostream. */
    template <typename CharT, typename Traits>
    inline friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& ost,
                                                                const xoshiro_base& e)
    {
        for (std::size_t i {}; i < e.state_.size(); ++i)
        {
            ost << e.state_[i] << ' ';
        }

        return ost;
    }

    /**  Reads a @c xorshiro from a @c std::istream. */
    template <typename CharT, typename Traits>
    inline friend std::basic_istream<CharT, Traits>& operator>>(std::basic_istream<CharT, Traits>& ist,
                                                                xoshiro_base& e)
    {
        for (std::size_t i {}; i < e.state_.size(); ++i)
        {
            ist >> e.state_[i] >> std::ws;
        }

        return ist;
    }

    /** Fills a range with random values */
    template <typename FIter>
    inline void generate(FIter first, FIter last) noexcept
    {
        using iter_type = typename std::iterator_traits<FIter>::value_type;

        while (first != last)
        {
            *first++ = static_cast<iter_type>(next());
        }
    }

    /**
     * Returns the largest value that the @c xorshiro
     * can produce.
     */
    static constexpr result_type (max)() noexcept
    {
        return (std::numeric_limits<result_type>::max)();
    }

    /**
     * Returns the smallest value that the @c xorshiro
     * can produce.
     */
    static constexpr result_type (min)() noexcept
    {
        return (std::numeric_limits<result_type>::min)();
    }

    inline std::array<BlockType, N> state() const noexcept
    {
        return state_;
    }
};

} // namespace detail
} // namespace random
} // namespace boost

#endif
