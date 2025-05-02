///////////////////////////////////////////////////////////////
//  Copyright Jens Maurer 2000-2021
//  Copyright Steven Watanabe 2011-2021
//  Copyright John Maddock 2015-2021
//  Copyright Matt Borland 2021
//  Distributed under the Boost Software License, Version 1.0. 
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  https://www.boost.org/LICENSE_1_0.txt
//
//  This is a C++11 compliant port of Boost.Random's implementation
//  of uniform_int_distribution. See their comments for detailed
//  descriptions

#ifndef BOOST_MP_UNIFORM_INT_DISTRIBUTION_HPP
#define BOOST_MP_UNIFORM_INT_DISTRIBUTION_HPP

#include <limits>
#include <type_traits>
#include <boost/multiprecision/detail/standalone_config.hpp>
#include <boost/multiprecision/detail/assert.hpp>
#include <boost/multiprecision/traits/std_integer_traits.hpp>

namespace boost { namespace multiprecision { 
    
namespace detail {

template <typename T, bool intrinsic>
struct make_unsigned_impl
{
    using type = typename boost::multiprecision::detail::make_unsigned<T>::type;
};

template <typename T>
struct make_unsigned_impl<T, false>
{
    using type = T;
};

template <typename T>
struct make_unsigned_mp
{
    using type = typename make_unsigned_impl<T, boost::multiprecision::detail::is_integral<T>::value>::type;
};

template <typename Engine, typename T>
T generate_uniform_int (Engine& eng, T min_value, T max_value)
{
    using range_type = typename boost::multiprecision::detail::make_unsigned_mp<T>::type;
    using base_result = typename Engine::result_type;
    using base_unsigned = typename boost::multiprecision::detail::make_unsigned_mp<base_result>::type;

    const range_type range = max_value - min_value;
    const base_result bmin = (eng.min)();
    const base_unsigned brange = (eng.max)() - (eng.min)();

    if(range == 0)
    {
        return min_value;
    }
    else if (brange < range)
    {
        for(;;)
        {
            range_type limit;
            if(range == (std::numeric_limits<range_type>::max)())
            {
                limit = range / (range_type(brange) + 1);
                if(range % (range_type(brange) + 1) == range_type(brange))
                {
                    ++limit;
                }
            }
            else
            {
                limit = (range + 1) / (range_type(brange) + 1);
            }

            range_type result = 0;
            range_type mult = 1;

            while (mult <= limit)
            {
                result += static_cast<range_type>(static_cast<range_type>(eng() - bmin) * mult);

                if(mult * range_type(brange) == range - mult + 1)
                {
                    return(result);
                }

                mult *= range_type(brange)+range_type(1);
            }

            range_type result_increment = generate_uniform_int(eng, range_type(0), range_type(range/mult));

            if(std::numeric_limits<range_type>::is_bounded && ((std::numeric_limits<range_type>::max)() / mult < result_increment))
            {
                continue;
            }

            result_increment *= mult;
            result += result_increment;

            if(result < result_increment)
            {
                continue;
            }
            if(result > range)
            {
                continue;
            }

            return result + min_value;
        }
    }
    else
    {
        using mixed_range_type = 
        typename std::conditional<std::numeric_limits<range_type>::is_specialized && std::numeric_limits<base_unsigned>::is_specialized &&
                                  (std::numeric_limits<range_type>::digits >= std::numeric_limits<base_unsigned>::digits),
                                  range_type, base_unsigned>::type;

        mixed_range_type bucket_size;

        if(brange == (std::numeric_limits<base_unsigned>::max)()) 
        {
            bucket_size = static_cast<mixed_range_type>(brange) / (static_cast<mixed_range_type>(range)+1);
            if(static_cast<mixed_range_type>(brange) % (static_cast<mixed_range_type>(range)+1) == static_cast<mixed_range_type>(range)) 
            {
                ++bucket_size;
            }
        } 
        else 
        {
            bucket_size = static_cast<mixed_range_type>(brange + 1) / (static_cast<mixed_range_type>(range)+1);
        }

        for(;;) 
        {
            mixed_range_type result = eng() - bmin;
            result /= bucket_size;

            if(result <= static_cast<mixed_range_type>(range))
            {
                return result + min_value;
            }
        }
    }
}

} // Namespace detail

template <typename Integer = int>
class uniform_int_distribution
{
private:
    Integer min_;
    Integer max_;

public:
    class param_type
    {
    private:
        Integer min_;
        Integer max_;
    
    public:
        explicit param_type(Integer min_val, Integer max_val) : min_ {min_val}, max_ {max_val}
        {
            BOOST_MP_ASSERT(min_ <= max_);
        }

        Integer a() const { return min_; }
        Integer b() const { return max_; }
    };

    explicit uniform_int_distribution(Integer min_arg, Integer max_arg) : min_ {min_arg}, max_ {max_arg}
    {
        BOOST_MP_ASSERT(min_ <= max_);
    }

    explicit uniform_int_distribution(const param_type& param_arg) : min_ {param_arg.a()}, max_ {param_arg.b()} {}

    Integer min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return min_; }
    Integer max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return max_; }
    
    Integer a() const { return min_; }
    Integer b() const { return max_; }

    param_type param() const { return param_type(min_, max_); }

    void param(const param_type& param_arg)
    {
        min_ = param_arg.a();
        max_ = param_arg.b();
    }

    template <typename Engine>
    Integer operator() (Engine& eng) const
    {
        return detail::generate_uniform_int(eng, min_, max_);
    }

    template <typename Engine>
    Integer operator() (Engine& eng, const param_type& param_arg) const
    {
        return detail::generate_uniform_int(eng, param_arg.a(), param_arg.b());
    }
};

}} // Namespaces

#endif // BOOST_MP_UNIFORM_INT_DISTRIBUTION_HPP
