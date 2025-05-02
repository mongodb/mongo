/* 
 * Copyright Sebastiano Vigna 2015.
 * Copyright Matt Borland 2022.
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 * 
 */

#ifndef BOOST_RANDOM_SPLITMIX64_HPP
#define BOOST_RANDOM_SPLITMIX64_HPP

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <array>
#include <string>
#include <ios>
#include <type_traits>

namespace boost { namespace random {

/**
 *  This is a fixed-increment version of Java 8's SplittableRandom generator
 *  See http://dx.doi.org/10.1145/2714064.2660195 and
 *  http://docs.oracle.com/javase/8/docs/api/java/util/SplittableRandom.html
 *  It is a very fast generator passing BigCrush, and it can be useful if
 *  for some reason you absolutely want 64 bits of state; otherwise, we
 *  rather suggest to use a xoroshiro128+ (for moderately parallel
 *  computations) or xorshift1024* (for massively parallel computations)
 *  generator.
 */
class splitmix64
{
private:
    std::uint64_t state_;

    inline std::uint64_t concatenate(std::uint32_t word1, std::uint32_t word2) noexcept
    {
        return static_cast<std::uint64_t>(word1) << 32 | word2;
    }

public:
    using result_type = std::uint64_t;
    using seed_type = std::uint64_t;

    // Required for old Boost.Random concept
    static constexpr bool has_fixed_range {false};

    /** Seeds the generator with the default seed. */
    void seed(result_type value = 0) noexcept
    {
        if (value == 0)
        {
            state_ = UINT64_C(0xA164B43C8F634A13);
        }
        else
        {
            state_ = value;
        }
    }

    /**
     * Seeds the generator with 32-bit values produced by @c seq.generate().
     */
    template <typename Sseq, typename std::enable_if<!std::is_convertible<Sseq, std::uint64_t>::value, bool>::type = true>
    void seed(Sseq& seq)
    {
        std::array<std::uint32_t, 2> seeds;
        seq.generate(seeds.begin(), seeds.end());

        state_ = concatenate(seeds[0], seeds[1]);
    }

    /**
     * Seeds the generator with 64-bit values produced by @c seq.generate().
     */
    template <typename Sseq, typename std::enable_if<!std::is_convertible<Sseq, splitmix64>::value, bool>::type = true>
    explicit splitmix64(Sseq& seq)
    {
        seed(seq);
    }

    /** Seeds the generator with a user provided seed. */
    template <typename T, typename std::enable_if<std::is_convertible<T, std::uint64_t>::value, bool>::type = true>
    void seed(T value = 0) noexcept
    {
        seed(static_cast<std::uint64_t>(value));
    }

    /** Seeds the generator with a user provided seed. */
    explicit splitmix64(std::uint64_t state = 0) noexcept
    {
        seed(state);
    }

    splitmix64(const splitmix64& other) = default;
    splitmix64& operator=(const splitmix64& other) = default;

    /**  Returns the next value of the generator. */
    inline result_type next() noexcept
    {
        std::uint64_t z {state_ += UINT64_C(0x9E3779B97F4A7C15)};
	    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
	    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
	    
        return z ^ (z >> 31);
    }

    /**  Returns the next value of the generator. */
    inline result_type operator()() noexcept
    {
        return next();
    }

    /** Advances the state of the generator by @c z. */
    inline void discard(std::uint64_t z) noexcept
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
    inline friend bool operator==(const splitmix64& lhs, const splitmix64& rhs) noexcept
    {
        return lhs.state_ == rhs.state_;
    }

    /**
     * Returns true if the two generators will produce different
     * sequences of values.
     */
    inline friend bool operator!=(const splitmix64& lhs, const splitmix64& rhs) noexcept
    {
        return !(lhs == rhs);
    }

    /**  Writes a @c splitmix64 to a @c std::ostream. */
    template <typename CharT, typename Traits>
    inline friend std::basic_ostream<CharT,Traits>& operator<<(std::basic_ostream<CharT,Traits>& ost, 
                                                               const splitmix64& e)
    {
        ost << e.state_;
        return ost;
    }

    /**  Writes a @c splitmix64 to a @c std::istream. */
    template <typename CharT, typename Traits>
    inline friend std::basic_istream<CharT,Traits>& operator>>(std::basic_istream<CharT,Traits>& ist,
                                                               splitmix64& e)
    {
        std::string sstate;
        CharT val;
        while (ist >> val)
        {
            if (std::isdigit(val))
            {
                sstate.push_back(val);
            }
        }
        
        e.state_ = std::strtoull(sstate.c_str(), nullptr, 10);

        return ist;
    }

    /** Fills a range with random values */
    template <typename FIter>
    inline void generate(FIter first, FIter last) noexcept
    {
        while (first != last)
        {
            *first++ = next();
        }
    }

    /**
     * Returns the largest value that the @c splitmix64
     * can produce.
     */
    static constexpr result_type (max)() noexcept
    {
        return (std::numeric_limits<std::uint64_t>::max)();
    }

    /**
     * Returns the smallest value that the @c splitmix64
     * can produce.
     */
    static constexpr result_type (min)() noexcept
    {
        return (std::numeric_limits<std::uint64_t>::min)();
    }
};

}} // Namespace boost::random

#endif // BOOST_RANDOM_SPLITMIX64_HPP
