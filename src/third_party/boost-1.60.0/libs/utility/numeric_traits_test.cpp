//  (C) Copyright David Abrahams 2001.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version including documentation.

//  Revision History
//  1  Apr 2001 Fixes for ICL; use BOOST_STATIC_CONSTANT
//  11 Feb 2001 Fixes for Borland (David Abrahams)
//  23 Jan 2001 Added test for wchar_t (David Abrahams)
//  23 Jan 2001 Now statically selecting a test for signed numbers to avoid
//              warnings with fancy compilers. Added commentary and
//              additional dumping of traits data for tested types (David
//              Abrahams).
//  21 Jan 2001 Initial version (David Abrahams)

#include <boost/detail/numeric_traits.hpp>
#include <cassert>
#include <boost/type_traits.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/utility.hpp>
#include <boost/lexical_cast.hpp>
#include <climits>
#include <typeinfo>
#include <iostream>
#include <string>
#ifndef BOOST_NO_LIMITS
# include <limits>
#endif

// =================================================================================
// template class complement_traits<Number> --
//
//    statically computes the max and min for 1s and 2s-complement binary
//    numbers. This helps on platforms without <limits> support. It also shows
//    an example of a recursive template that works with MSVC!
//

template <unsigned size> struct complement; // forward

// The template complement, below, does all the real work, using "poor man's
// partial specialization". We need complement_traits_aux<> so that MSVC doesn't
// complain about undefined min/max as we're trying to recursively define them. 
template <class Number, unsigned size>
struct complement_traits_aux
{
    BOOST_STATIC_CONSTANT(Number, max = complement<size>::template traits<Number>::max);
    BOOST_STATIC_CONSTANT(Number, min = complement<size>::template traits<Number>::min);
};

template <unsigned size>
struct complement
{
    template <class Number>
    struct traits
    {
     private:
        // indirection through complement_traits_aux necessary to keep MSVC happy
        typedef complement_traits_aux<Number, size - 1> prev;
     public:
#if defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 0 && __GNUC_PATCHLEVEL__ == 2
        // GCC 4.0.2 ICEs on these C-style casts
        BOOST_STATIC_CONSTANT(Number, max =
                            Number((prev::max) << CHAR_BIT)
                            + Number(UCHAR_MAX));
        BOOST_STATIC_CONSTANT(Number, min = Number((prev::min) << CHAR_BIT));
#else
        // Avoid left shifting negative integers, use multiplication instead
        BOOST_STATIC_CONSTANT(Number, shift = 1u << CHAR_BIT);
        BOOST_STATIC_CONSTANT(Number, max =
                            Number(Number(prev::max) * shift)
                            + Number(UCHAR_MAX));
        BOOST_STATIC_CONSTANT(Number, min = Number(Number(prev::min) * shift));
#endif
    };
};

// Template class complement_base<> -- defines values for min and max for
// complement<1>, at the deepest level of recursion. Uses "poor man's partial
// specialization" again.
template <bool is_signed> struct complement_base;

template <> struct complement_base<false>
{
    template <class Number>
    struct values
    {
        BOOST_STATIC_CONSTANT(Number, min = 0);
        BOOST_STATIC_CONSTANT(Number, max = UCHAR_MAX);
    };
};

template <> struct complement_base<true>
{
    template <class Number>
    struct values
    {
        BOOST_STATIC_CONSTANT(Number, min = SCHAR_MIN);
        BOOST_STATIC_CONSTANT(Number, max = SCHAR_MAX);
    };
};

// Base specialization of complement, puts an end to the recursion.
template <>
struct complement<1>
{
    template <class Number>
    struct traits
    {
        BOOST_STATIC_CONSTANT(bool, is_signed = boost::detail::is_signed<Number>::value);
        BOOST_STATIC_CONSTANT(Number, min =
                            complement_base<is_signed>::template values<Number>::min);
        BOOST_STATIC_CONSTANT(Number, max =
                            complement_base<is_signed>::template values<Number>::max);
    };
};

// Now here's the "pretty" template you're intended to actually use.
//   complement_traits<Number>::min, complement_traits<Number>::max are the
//   minimum and maximum values of Number if Number is a built-in integer type.
template <class Number>
struct complement_traits
{
    BOOST_STATIC_CONSTANT(Number, max = (complement_traits_aux<Number, sizeof(Number)>::max));
    BOOST_STATIC_CONSTANT(Number, min = (complement_traits_aux<Number, sizeof(Number)>::min));
};

// =================================================================================

// Support for streaming various numeric types in exactly the format I want. I
// needed this in addition to all the assertions so that I could see exactly
// what was going on.
//
// Numbers go through a 2-stage conversion process (by default, though, no real
// conversion).
//
template <class T> struct stream_as {
    typedef T t1;
    typedef T t2;
};

// char types first get converted to unsigned char, then to unsigned.
template <> struct stream_as<char> {
    typedef unsigned char t1;
    typedef unsigned t2;
};
template <> struct stream_as<unsigned char> {
    typedef unsigned char t1; typedef unsigned t2;
};
template <> struct stream_as<signed char>  {
    typedef unsigned char t1; typedef unsigned t2;
};

#if defined(BOOST_MSVC_STD_ITERATOR) // No intmax streaming built-in

// With this library implementation, __int64 and __uint64 get streamed as strings
template <> struct stream_as<boost::uintmax_t> {
    typedef std::string t1;
    typedef std::string t2;
};

template <> struct stream_as<boost::intmax_t>  {
    typedef std::string t1;
    typedef std::string t2;
};
#endif

// Standard promotion process for streaming
template <class T> struct promote
{
    static typename stream_as<T>::t1 from(T x) {
        typedef typename stream_as<T>::t1 t1;
        return t1(x);
    }
};

#if defined(BOOST_MSVC_STD_ITERATOR) // No intmax streaming built-in

// On this platform, stream them as long/unsigned long if they fit.
// Otherwise, write a string.
template <> struct promote<boost::uintmax_t> {
    std::string static from(const boost::uintmax_t x) {
        if (x > ULONG_MAX)
            return std::string("large unsigned value");
        else
            return boost::lexical_cast<std::string>((unsigned long)x);
    }
};
template <> struct promote<boost::intmax_t> {
    std::string static from(const boost::intmax_t x) {
        if (x > boost::intmax_t(ULONG_MAX))
            return std::string("large positive signed value");
        else if (x >= 0)
            return boost::lexical_cast<std::string>((unsigned long)x);
        
        if (x < boost::intmax_t(LONG_MIN))
            return std::string("large negative signed value");
        else
            return boost::lexical_cast<std::string>((long)x);
    }
};
#endif

// This is the function which converts types to the form I want to stream them in.
template <class T>
typename stream_as<T>::t2 stream_number(T x)
{
    return promote<T>::from(x);
}
// =================================================================================

//
// Tests for built-in signed and unsigned types
//

// Tag types for selecting tests
struct unsigned_tag {};
struct signed_tag {};

// Tests for unsigned numbers. The extra default Number parameter works around
// an MSVC bug.
template <class Number>
void test_aux(unsigned_tag, Number*)
{
    typedef typename boost::detail::numeric_traits<Number>::difference_type difference_type;
    BOOST_STATIC_ASSERT(!boost::detail::is_signed<Number>::value);
    BOOST_STATIC_ASSERT(
        (sizeof(Number) < sizeof(boost::intmax_t))
        | (boost::is_same<difference_type, boost::intmax_t>::value));

#if defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 0 && __GNUC_PATCHLEVEL__ == 2
    // GCC 4.0.2 ICEs on this C-style cases
    BOOST_STATIC_ASSERT((complement_traits<Number>::max) > Number(0));
    BOOST_STATIC_ASSERT((complement_traits<Number>::min) == Number(0));
#else
    // Force casting to Number here to work around the fact that it's an enum on MSVC
    BOOST_STATIC_ASSERT(Number(complement_traits<Number>::max) > Number(0));
    BOOST_STATIC_ASSERT(Number(complement_traits<Number>::min) == Number(0));
#endif

    const Number max = complement_traits<Number>::max;
    const Number min = complement_traits<Number>::min;
    
    const Number test_max = (sizeof(Number) < sizeof(boost::intmax_t))
        ? max
        : max / 2 - 1;

    std::cout << std::hex << "(unsigned) min = " << stream_number(min) << ", max = "
              << stream_number(max) << "..." << std::flush;
    std::cout << "difference_type = " << typeid(difference_type).name() << "..."
              << std::flush;
    
    difference_type d1 = boost::detail::numeric_distance(Number(0), test_max);
    difference_type d2 = boost::detail::numeric_distance(test_max, Number(0));
    
    std::cout << "0->" << stream_number(test_max) << "==" << std::dec << stream_number(d1) << "; "
              << std::hex << stream_number(test_max) << "->0==" << std::dec << stream_number(d2) << "..." << std::flush;

    assert(d1 == difference_type(test_max));
    assert(d2 == -difference_type(test_max));
}

// Tests for signed numbers. The extra default Number parameter works around an
// MSVC bug.
struct out_of_range_tag {};
struct in_range_tag {};

// This test morsel gets executed for numbers whose difference will always be
// representable in intmax_t
template <class Number>
void signed_test(in_range_tag, Number*)
{
    BOOST_STATIC_ASSERT(boost::detail::is_signed<Number>::value);
    typedef typename boost::detail::numeric_traits<Number>::difference_type difference_type;
    const Number max = complement_traits<Number>::max;
    const Number min = complement_traits<Number>::min;
    
    difference_type d1 = boost::detail::numeric_distance(min, max);
    difference_type d2 = boost::detail::numeric_distance(max, min);

    std::cout << stream_number(min) << "->" << stream_number(max) << "==";
    std::cout << std::dec << stream_number(d1) << "; ";
    std::cout << std::hex << stream_number(max) << "->" << stream_number(min)
              << "==" << std::dec << stream_number(d2) << "..." << std::flush;
    assert(d1 == difference_type(max) - difference_type(min));
    assert(d2 == difference_type(min) - difference_type(max));
}

// This test morsel gets executed for numbers whose difference may exceed the
// capacity of intmax_t.
template <class Number>
void signed_test(out_of_range_tag, Number*)
{
    BOOST_STATIC_ASSERT(boost::detail::is_signed<Number>::value);
    typedef typename boost::detail::numeric_traits<Number>::difference_type difference_type;
    const Number max = complement_traits<Number>::max;
    const Number min = complement_traits<Number>::min;

    difference_type min_distance = complement_traits<difference_type>::min;
    difference_type max_distance = complement_traits<difference_type>::max;

    const Number n1 = Number(min + max_distance);
    const Number n2 = Number(max + min_distance);
    difference_type d1 = boost::detail::numeric_distance(min, n1);
    difference_type d2 = boost::detail::numeric_distance(max, n2);

    std::cout << stream_number(min) << "->" << stream_number(n1) << "==";
    std::cout << std::dec << stream_number(d1) << "; ";
    std::cout << std::hex << stream_number(max) << "->" << stream_number(n2)
              << "==" << std::dec << stream_number(d2) << "..." << std::flush;
    assert(d1 == max_distance);
    assert(d2 == min_distance);
}

template <class Number>
void test_aux(signed_tag, Number*)
{
    typedef typename boost::detail::numeric_traits<Number>::difference_type difference_type;
    BOOST_STATIC_ASSERT(boost::detail::is_signed<Number>::value);
    BOOST_STATIC_ASSERT(
        (sizeof(Number) < sizeof(boost::intmax_t))
        | (boost::is_same<difference_type, Number>::value));

#if defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 0 && __GNUC_PATCHLEVEL__ == 2
    // GCC 4.0.2 ICEs on this cast
    BOOST_STATIC_ASSERT((complement_traits<Number>::max) > Number(0));
    BOOST_STATIC_ASSERT((complement_traits<Number>::min) < Number(0));
#else
    // Force casting to Number here to work around the fact that it's an enum on MSVC
    BOOST_STATIC_ASSERT(Number(complement_traits<Number>::max) > Number(0));
    BOOST_STATIC_ASSERT(Number(complement_traits<Number>::min) < Number(0));
#endif    
    const Number max = complement_traits<Number>::max;
    const Number min = complement_traits<Number>::min;
    
    std::cout << std::hex << "min = " << stream_number(min) << ", max = "
              << stream_number(max) << "..." << std::flush;
    std::cout << "difference_type = " << typeid(difference_type).name() << "..."
              << std::flush;

    typedef typename boost::detail::if_true<
                          (sizeof(Number) < sizeof(boost::intmax_t))>
                        ::template then<
                          in_range_tag,
                          out_of_range_tag
                        >::type
        range_tag;
    signed_test<Number>(range_tag(), 0);
}


// Test for all numbers. The extra default Number parameter works around an MSVC
// bug.
template <class Number>
void test(Number* = 0)
{
    std::cout << "testing " << typeid(Number).name() << ":\n"
#ifndef BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
              << "is_signed: " << (std::numeric_limits<Number>::is_signed ? "true\n" : "false\n")
              << "is_bounded: " << (std::numeric_limits<Number>::is_bounded ? "true\n" : "false\n")
              << "digits: " << std::numeric_limits<Number>::digits << "\n"
#endif
              << "..." << std::flush;

    // factoring out difference_type for the assert below confused Borland :(
    typedef boost::detail::is_signed<
#if !defined(BOOST_MSVC) || BOOST_MSVC > 1300
        typename
#endif
        boost::detail::numeric_traits<Number>::difference_type
        > is_signed;
    BOOST_STATIC_ASSERT(is_signed::value);

    typedef typename boost::detail::if_true<
        boost::detail::is_signed<Number>::value
        >::template then<signed_tag, unsigned_tag>::type signedness;
    
    test_aux<Number>(signedness(), 0);
    std::cout << "passed" << std::endl;
}

int main()
{
    test<char>();
    test<unsigned char>();
    test<signed char>();
    test<wchar_t>();
    test<short>();
    test<unsigned short>();
    test<int>();
    test<unsigned int>();
    test<long>();
    test<unsigned long>();
#if defined(BOOST_HAS_LONG_LONG) && !defined(BOOST_NO_INTEGRAL_INT64_T)
    test< ::boost::long_long_type>();
    test< ::boost::ulong_long_type>();
#elif defined(BOOST_MSVC)
    // The problem of not having compile-time static class constants other than
    // enums prevents this from working, since values get truncated.
    // test<boost::uintmax_t>();
    // test<boost::intmax_t>();
#endif
    return 0;
}
