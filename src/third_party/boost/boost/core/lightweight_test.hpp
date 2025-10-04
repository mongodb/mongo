#ifndef BOOST_CORE_LIGHTWEIGHT_TEST_HPP
#define BOOST_CORE_LIGHTWEIGHT_TEST_HPP

// MS compatible compilers support #pragma once

#if defined(_MSC_VER)
# pragma once
#endif

//
//  boost/core/lightweight_test.hpp - lightweight test library
//
//  Copyright (c) 2002, 2009, 2014 Peter Dimov
//  Copyright (2) Beman Dawes 2010, 2011
//  Copyright (3) Ion Gaztanaga 2013
//
//  Copyright 2018 Glen Joseph Fernandes
//  (glenjofe@gmail.com)
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt
//

#include <boost/core/detail/lwt_unattended.hpp>
#include <boost/current_function.hpp>
#include <boost/config.hpp>
#include <exception>
#include <iostream>
#include <iterator>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <cctype>

//  IDE's like Visual Studio perform better if output goes to std::cout or
//  some other stream, so allow user to configure output stream:
#ifndef BOOST_LIGHTWEIGHT_TEST_OSTREAM
# define BOOST_LIGHTWEIGHT_TEST_OSTREAM std::cerr
#endif

namespace boost
{
namespace detail
{

class test_result
{
public:

    test_result(): report_( false ), errors_( 0 )
    {
        core::detail::lwt_unattended();
    }

    ~test_result()
    {
        if( !report_ )
        {
            BOOST_LIGHTWEIGHT_TEST_OSTREAM << "main() should return report_errors()" << std::endl;
            std::abort();
        }
    }

    int& errors()
    {
        return errors_;
    }

    void done()
    {
        report_ = true;
    }

private:

    bool report_;
    int errors_;
};

inline test_result& test_results()
{
    static test_result instance;
    return instance;
}

inline int& test_errors()
{
    return test_results().errors();
}

inline bool test_impl(char const * expr, char const * file, int line, char const * function, bool v)
{
    if( v )
    {
        test_results();
        return true;
    }
    else
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
          << file << "(" << line << "): test '" << expr << "' failed in function '"
          << function << "'" << std::endl;
        ++test_results().errors();
        return false;
    }
}

inline void error_impl(char const * msg, char const * file, int line, char const * function)
{
    BOOST_LIGHTWEIGHT_TEST_OSTREAM
      << file << "(" << line << "): " << msg << " in function '"
      << function << "'" << std::endl;
    ++test_results().errors();
}

inline void throw_failed_impl(const char* expr, char const * excep, char const * file, int line, char const * function)
{
   BOOST_LIGHTWEIGHT_TEST_OSTREAM
    << file << "(" << line << "): expression '" << expr << "' did not throw exception '" << excep << "' in function '"
    << function << "'" << std::endl;
   ++test_results().errors();
}

inline void no_throw_failed_impl(const char* expr, const char* file, int line, const char* function)
{
    BOOST_LIGHTWEIGHT_TEST_OSTREAM
        << file << "(" << line << "): expression '" << expr << "' threw an exception in function '"
        << function << "'" << std::endl;
   ++test_results().errors();
}

inline void no_throw_failed_impl(const char* expr, const char* what, const char* file, int line, const char* function)
{
    BOOST_LIGHTWEIGHT_TEST_OSTREAM
        << file << "(" << line << "): expression '" << expr << "' threw an exception in function '"
        << function << "': " << what << std::endl;
   ++test_results().errors();
}

// In the comparisons below, it is possible that T and U are signed and unsigned integer types, which generates warnings in some compilers.
// A cleaner fix would require common_type trait or some meta-programming, which would introduce a dependency on Boost.TypeTraits. To avoid
// the dependency we just disable the warnings.
#if defined(__clang__) && defined(__has_warning)
# if __has_warning("-Wsign-compare")
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wsign-compare"
# endif
#elif defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4389)
#elif defined(__GNUC__) && !(defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)) && (__GNUC__ * 100 + __GNUC_MINOR__) >= 406
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wsign-compare"
# pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

// specialize test output for char pointers to avoid printing as cstring
template <class T> inline const T& test_output_impl(const T& v) { return v; }
inline const void* test_output_impl(const char* v) { return v; }
inline const void* test_output_impl(const unsigned char* v) { return v; }
inline const void* test_output_impl(const signed char* v) { return v; }
inline const void* test_output_impl(char* v) { return v; }
inline const void* test_output_impl(unsigned char* v) { return v; }
inline const void* test_output_impl(signed char* v) { return v; }
template<class T> inline const void* test_output_impl(T volatile* v) { return const_cast<T*>(v); }

#if !defined( BOOST_NO_CXX11_NULLPTR )
inline const void* test_output_impl(std::nullptr_t) { return nullptr; }
#endif

// print chars as numeric

inline int test_output_impl( signed char const& v ) { return v; }
inline unsigned test_output_impl( unsigned char const& v ) { return v; }

// Whether wchar_t is signed is implementation-defined

template<bool Signed> struct lwt_long_type {};
template<> struct lwt_long_type<true> { typedef long type; };
template<> struct lwt_long_type<false> { typedef unsigned long type; };

inline lwt_long_type<(static_cast<wchar_t>(-1) < static_cast<wchar_t>(0))>::type test_output_impl( wchar_t const& v ) { return v; }

#if !defined( BOOST_NO_CXX11_CHAR16_T )
inline unsigned long test_output_impl( char16_t const& v ) { return v; }
#endif

#if !defined( BOOST_NO_CXX11_CHAR32_T )
inline unsigned long test_output_impl( char32_t const& v ) { return v; }
#endif

inline std::string test_output_impl( char const& v )
{
    if( std::isprint( static_cast<unsigned char>( v ) ) )
    {
        return std::string( 1, v );
    }
    else
    {
        static const char char_table[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
        char buffer[ 4 ];
        buffer[ 0 ] = '\\';
        buffer[ 1 ] = 'x';
        buffer[ 2 ] = char_table[ (static_cast<unsigned char>( v ) >> 4u) & 0x0f ];
        buffer[ 3 ] = char_table[ static_cast<unsigned char>( v ) & 0x0f ];

        return std::string( buffer, 4u );
    }
}

// predicates

struct lw_test_eq
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t == u; }
};

struct lw_test_ne
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t != u; }
};

struct lw_test_lt
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t < u; }
};

struct lw_test_le
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t <= u; }
};

struct lw_test_gt
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t > u; }
};

struct lw_test_ge
{
    template <typename T, typename U>
    bool operator()(const T& t, const U& u) const { return t >= u; }
};

// lwt_predicate_name

template<class T> char const * lwt_predicate_name( T const& )
{
    return "~=";
}

inline char const * lwt_predicate_name( lw_test_eq const& )
{
    return "==";
}

inline char const * lwt_predicate_name( lw_test_ne const& )
{
    return "!=";
}

inline char const * lwt_predicate_name( lw_test_lt const& )
{
    return "<";
}

inline char const * lwt_predicate_name( lw_test_le const& )
{
    return "<=";
}

inline char const * lwt_predicate_name( lw_test_gt const& )
{
    return ">";
}

inline char const * lwt_predicate_name( lw_test_ge const& )
{
    return ">=";
}

//

template<class BinaryPredicate, class T, class U>
inline bool test_with_impl(BinaryPredicate pred, char const * expr1, char const * expr2,
                           char const * file, int line, char const * function,
                           T const & t, U const & u)
{
    if( pred(t, u) )
    {
        test_results();
        return true;
    }
    else
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << file << "(" << line << "): test '" << expr1 << " " << lwt_predicate_name(pred) << " " << expr2
            << "' ('" << test_output_impl(t) << "' " << lwt_predicate_name(pred) << " '" << test_output_impl(u)
            << "') failed in function '" << function << "'" << std::endl;
        ++test_results().errors();
        return false;
    }
}

inline bool test_cstr_eq_impl( char const * expr1, char const * expr2,
  char const * file, int line, char const * function, char const * const t, char const * const u )
{
    if( std::strcmp(t, u) == 0 )
    {
        test_results();
        return true;
    }
    else
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << file << "(" << line << "): test '" << expr1 << " == " << expr2 << "' ('" << t
            << "' == '" << u << "') failed in function '" << function << "'" << std::endl;
        ++test_results().errors();
        return false;
    }
}

inline bool test_cstr_ne_impl( char const * expr1, char const * expr2,
  char const * file, int line, char const * function, char const * const t, char const * const u )
{
    if( std::strcmp(t, u) != 0 )
    {
        test_results();
        return true;
    }
    else
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
            << file << "(" << line << "): test '" << expr1 << " != " << expr2 << "' ('" << t
            << "' != '" << u << "') failed in function '" << function << "'" << std::endl;
        ++test_results().errors();
        return false;
    }
}

template<class FormattedOutputFunction, class InputIterator1, class InputIterator2>
bool test_all_eq_impl(FormattedOutputFunction& output,
                      char const * file, int line, char const * function,
                      InputIterator1 first_begin, InputIterator1 first_end,
                      InputIterator2 second_begin, InputIterator2 second_end)
{
    InputIterator1 first_it = first_begin;
    InputIterator2 second_it = second_begin;
    typename std::iterator_traits<InputIterator1>::difference_type first_index = 0;
    typename std::iterator_traits<InputIterator2>::difference_type second_index = 0;
    std::size_t error_count = 0;
    const std::size_t max_count = 8;
    do
    {
        while ((first_it != first_end) && (second_it != second_end) && (*first_it == *second_it))
        {
            ++first_it;
            ++second_it;
            ++first_index;
            ++second_index;
        }
        if ((first_it == first_end) || (second_it == second_end))
        {
            break; // do-while
        }
        if (error_count == 0)
        {
            output << file << "(" << line << "): Container contents differ in function '" << function << "':";
        }
        else if (error_count >= max_count)
        {
            output << " ...";
            break;
        }
        output << " [" << first_index << "] '" << test_output_impl(*first_it) << "' != '" << test_output_impl(*second_it) << "'";
        ++first_it;
        ++second_it;
        ++first_index;
        ++second_index;
        ++error_count;
    } while (first_it != first_end);

    first_index += std::distance(first_it, first_end);
    second_index += std::distance(second_it, second_end);
    if (first_index != second_index)
    {
        if (error_count == 0)
        {
            output << file << "(" << line << "): Container sizes differ in function '" << function << "': size(" << first_index << ") != size(" << second_index << ")";
        }
        else
        {
            output << " [*] size(" << first_index << ") != size(" << second_index << ")";
        }
        ++error_count;
    }

    if (error_count == 0)
    {
        test_results();
        return true;
    }
    else
    {
        output << std::endl;
        ++test_results().errors();
        return false;
    }
}

template<class FormattedOutputFunction, class InputIterator1, class InputIterator2, typename BinaryPredicate>
bool test_all_with_impl(FormattedOutputFunction& output,
                        char const * file, int line, char const * function,
                        InputIterator1 first_begin, InputIterator1 first_end,
                        InputIterator2 second_begin, InputIterator2 second_end,
                        BinaryPredicate predicate)
{
    InputIterator1 first_it = first_begin;
    InputIterator2 second_it = second_begin;
    typename std::iterator_traits<InputIterator1>::difference_type first_index = 0;
    typename std::iterator_traits<InputIterator2>::difference_type second_index = 0;
    std::size_t error_count = 0;
    const std::size_t max_count = 8;
    do
    {
        while ((first_it != first_end) && (second_it != second_end) && predicate(*first_it, *second_it))
        {
            ++first_it;
            ++second_it;
            ++first_index;
            ++second_index;
        }
        if ((first_it == first_end) || (second_it == second_end))
        {
            break; // do-while
        }
        if (error_count == 0)
        {
            output << file << "(" << line << "): Container contents differ in function '" << function << "':";
        }
        else if (error_count >= max_count)
        {
            output << " ...";
            break;
        }
        output << " [" << first_index << "]";
        ++first_it;
        ++second_it;
        ++first_index;
        ++second_index;
        ++error_count;
    } while (first_it != first_end);

    first_index += std::distance(first_it, first_end);
    second_index += std::distance(second_it, second_end);
    if (first_index != second_index)
    {
        if (error_count == 0)
        {
            output << file << "(" << line << "): Container sizes differ in function '" << function << "': size(" << first_index << ") != size(" << second_index << ")";
        }
        else
        {
            output << " [*] size(" << first_index << ") != size(" << second_index << ")";
        }
        ++error_count;
    }

    if (error_count == 0)
    {
        test_results();
        return true;
    }
    else
    {
        output << std::endl;
        ++test_results().errors();
        return false;
    }
}

#if defined(__clang__) && defined(__has_warning)
# if __has_warning("-Wsign-compare")
#  pragma clang diagnostic pop
# endif
#elif defined(_MSC_VER)
# pragma warning(pop)
#elif defined(__GNUC__) && !(defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)) && (__GNUC__ * 100 + __GNUC_MINOR__) >= 406
# pragma GCC diagnostic pop
#endif

} // namespace detail

inline int report_errors()
{
    boost::detail::test_result& result = boost::detail::test_results();
    result.done();

    int errors = result.errors();

    if( errors == 0 )
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
          << "No errors detected." << std::endl;
    }
    else
    {
        BOOST_LIGHTWEIGHT_TEST_OSTREAM
          << errors << " error" << (errors == 1? "": "s") << " detected." << std::endl;
    }

    // `return report_errors();` from main only supports 8 bit exit codes
    return errors < 256? errors: 255;
}

namespace core
{

inline void lwt_init()
{
    boost::detail::test_results();
}

} // namespace core
} // namespace boost

#define BOOST_TEST(expr) ( ::boost::detail::test_impl(#expr, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, (expr)? true: false) )
#define BOOST_TEST_NOT(expr) BOOST_TEST(!(expr))

#define BOOST_ERROR(msg) ( ::boost::detail::error_impl(msg, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION) )

#define BOOST_TEST_WITH(expr1,expr2,predicate) ( ::boost::detail::test_with_impl(predicate, #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )

#define BOOST_TEST_EQ(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_eq(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )
#define BOOST_TEST_NE(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_ne(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )

#define BOOST_TEST_LT(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_lt(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )
#define BOOST_TEST_LE(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_le(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )
#define BOOST_TEST_GT(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_gt(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )
#define BOOST_TEST_GE(expr1,expr2) ( ::boost::detail::test_with_impl(::boost::detail::lw_test_ge(), #expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )

#define BOOST_TEST_CSTR_EQ(expr1,expr2) ( ::boost::detail::test_cstr_eq_impl(#expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )
#define BOOST_TEST_CSTR_NE(expr1,expr2) ( ::boost::detail::test_cstr_ne_impl(#expr1, #expr2, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, expr1, expr2) )

#define BOOST_TEST_ALL_EQ(begin1, end1, begin2, end2) ( ::boost::detail::test_all_eq_impl(BOOST_LIGHTWEIGHT_TEST_OSTREAM, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, begin1, end1, begin2, end2) )
#define BOOST_TEST_ALL_WITH(begin1, end1, begin2, end2, predicate) ( ::boost::detail::test_all_with_impl(BOOST_LIGHTWEIGHT_TEST_OSTREAM, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION, begin1, end1, begin2, end2, predicate) )

#ifndef BOOST_NO_EXCEPTIONS
   #define BOOST_TEST_THROWS( EXPR, EXCEP )                           \
      try {                                                           \
         EXPR;                                                        \
         ::boost::detail::throw_failed_impl                           \
         (#EXPR, #EXCEP, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION); \
      }                                                               \
      catch(EXCEP const&) {                                           \
         ::boost::detail::test_results();                             \
      }                                                               \
      catch(...) {                                                    \
         ::boost::detail::throw_failed_impl                           \
         (#EXPR, #EXCEP, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION); \
      }                                                               \
   //
#else
   #define BOOST_TEST_THROWS( EXPR, EXCEP )
#endif

#ifndef BOOST_NO_EXCEPTIONS
#  define BOOST_TEST_NO_THROW(EXPR)                                    \
    try {                                                              \
        EXPR;                                                          \
    } catch (const std::exception& e) {                                \
        ::boost::detail::no_throw_failed_impl                          \
        (#EXPR, e.what(), __FILE__, __LINE__, BOOST_CURRENT_FUNCTION); \
    } catch (...) {                                                    \
        ::boost::detail::no_throw_failed_impl                          \
        (#EXPR, __FILE__, __LINE__, BOOST_CURRENT_FUNCTION);           \
    }
    //
#else
#  define BOOST_TEST_NO_THROW(EXPR) { EXPR; }
#endif

#endif // #ifndef BOOST_CORE_LIGHTWEIGHT_TEST_HPP
