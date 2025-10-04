// ------------------------------------------------------------------------------
//  format_matrix.cpp : tool to check for regressions between boost format
//                      releases and compare format specification handling
// ------------------------------------------------------------------------------

//  Copyright 2017 - 2019 James E. King, III. Use, modification, and distribution 
//  are subject to the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/format for library home page

// ------------------------------------------------------------------------------
// reference (ISO C99)   : http://en.cppreference.com/w/cpp/io/c/fprintf
// reference (Java)      : http://docs.oracle.com/javase/8/docs/api/java/util/Formatter.html
// reference (Microsoft) : https://docs.microsoft.com/en-us/cpp/c-runtime-library/format-specification-syntax-printf-and-wprintf-functions
// reference (POSIX 2008): http://pubs.opengroup.org/onlinepubs/9699919799/functions/printf.html

#include <boost/array.hpp>
#include <boost/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/io/ios_state.hpp>
#include <boost/predef.h>
#include <boost/program_options.hpp>
#include <cerrno>
#include <climits>
#include <clocale>
#if BOOST_COMP_MSVC && BOOST_VERSION_NUMBER_MAJOR(BOOST_COMP_MSVC) >= 19
#include <corecrt.h>    // wint_t
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <math.h>

#define SNPRINTF snprintf
#if BOOST_COMP_MSVC && BOOST_VERSION_NUMBER_MAJOR(BOOST_COMP_MSVC) < 19
#undef SNPRINTF
#define SNPRINTF _snprintf
#endif

namespace fs = boost::filesystem;
namespace po = boost::program_options;
using namespace std;

namespace matrix
{

enum interop_datatype
{
    // special types:
    ID_UNDEF,           // undefined behavior according to the spec, so combination is not tested
    ID_NOTSUP,          // behavior is not supported and therefore not tested currently

    // boolean type values:
    ID_BOOLF,           // false
    ID_BOOLT,           // true

    // string type values:
    ID_CHAR,            // single signed character
    ID_UCHAR,           // single unsigned character
    ID_WCHAR,           // single wide character
    ID_STR,             // C style string
    ID_WSTR,            // C style wide string

    // integer type values:
    ID_ZERO,            // zero value
    ID_BYTE,            // int8_t
    ID_UBYTE,           // uint8_t
    ID_SHORT,           // signed short
    ID_USHORT,          // unsigned short
    ID_INT,             // signed integer
    ID_UINT,            // unsigned integer
    ID_LONG,            // signed long
    ID_ULONG,           // unsigned long
    ID_LLONG,           // signed long long
    ID_ULLONG,          // unsigned long long
    ID_INTMAX,          // intmax_t
    ID_SSIZET,          // ssize_t
    ID_SIZET,           // size_t
    ID_SPTRDF,          // signed ptrdiff_t
    ID_PTRDIF,          // ptrdiff_t

    // floating type values:
    ID_INF,             // infinity
    ID_NAN,             // not a number
    ID_DOUBLE,          // double
    ID_NEGDBL,          // negative double
    ID_LNGDBL,          // long double
    ID_NEGLNG           // negative long double
};

BOOST_CONSTEXPR const bool            g_bf  = false;
BOOST_CONSTEXPR const bool            g_bt  = true;
BOOST_CONSTEXPR const uint64_t        g_z   = 0;
BOOST_CONSTEXPR const char            g_by  = 0x60;
BOOST_CONSTEXPR const unsigned char   g_uby = 0xA0;
BOOST_CONSTEXPR const char            g_c   = 0x58;
BOOST_CONSTEXPR const wint_t          g_wc  = L'X';          // 'X', but wide
BOOST_CONSTEXPR const char *          g_s   = " string"; 
BOOST_CONSTEXPR const wchar_t *       g_ws  = L"widestr";
BOOST_CONSTEXPR const short           g_h   = numeric_limits<short>::min() + 12345;
BOOST_CONSTEXPR const unsigned short  g_uh  = numeric_limits<unsigned short>::max() - 12345;
BOOST_CONSTEXPR const int             g_i   = numeric_limits<int>::max() - 12345;
BOOST_CONSTEXPR const unsigned int    g_ui  = numeric_limits<unsigned int>::max() - 12345;
BOOST_CONSTEXPR const long            g_l   = numeric_limits<long>::max() - 12345;
BOOST_CONSTEXPR const unsigned long   g_ul  = numeric_limits<unsigned long>::max() - 12345;
BOOST_CONSTEXPR const int64_t         g_ll  = numeric_limits<int64_t>::max() - 12345;
BOOST_CONSTEXPR const uint64_t        g_ull = numeric_limits<uint64_t>::max() - 12345;
BOOST_CONSTEXPR const intmax_t        g_max = numeric_limits<intmax_t>::max() - 12345;
BOOST_CONSTEXPR const size_t          g_sst = numeric_limits<size_t>::min() - 12345;
BOOST_CONSTEXPR const size_t          g_st  = numeric_limits<size_t>::max() - 12345;
BOOST_CONSTEXPR const ptrdiff_t       g_pt  = numeric_limits<ptrdiff_t>::max() - 12345;
BOOST_CONSTEXPR const double          g_db  = 1234567.891234f;
BOOST_CONSTEXPR const double          g_ndb = -1234567.891234f;
BOOST_CONSTEXPR const long double     g_ldb = 6543211234567.891234l;
BOOST_CONSTEXPR const long double     g_nld = -6543211234567.891234l;

boost::array<const char *, 12> length_modifier = { { "hh", "h", "", "l", "ll", "j", "z", "L", "w", "I", "I32", "I64" } };
boost::array<const char *, 6>  format_flags    = { { "", "-", "+", " ", "#", "0" } };
boost::array<const char *, 6>  minimum_width   = { { "", "1", "2", "5", "10", "20" } };           // TODO: , "*" } };
boost::array<const char *, 7>  precision       = { { "", ".", ".0", ".2", ".5", ".10", ".20" } }; // TODO: , ".*" } };

struct interop_row
{
    char conversion_specifier;
    interop_datatype datatype[12];
};

// Each row represents a conversion specifier which is indicated in the first column
// The subsequent columns are argument type specifiers for that conversion specifier
// The data in the cell is the value to pass into snprintf and format to see what comes out

interop_row interop_matrix[] = {
    //         |----------------------------------- ISO C99 ---------------------------------------|   |-------------- Microsoft --------------|
    //  spc,   hh       , h        , (none)   , l        , ll       , j        , z        , L        , w        , I        , I32      , I64 
      { 'c', { ID_UNDEF , ID_UNDEF , ID_CHAR  , ID_WCHAR , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_WCHAR , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 's', { ID_UNDEF , ID_UNDEF , ID_STR   , ID_WSTR  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_WSTR  , ID_UNDEF , ID_UNDEF , ID_UNDEF } },

      { 'd', { ID_BYTE  , ID_SHORT , ID_INT   , ID_LONG  , ID_LLONG , ID_INTMAX, ID_SSIZET, ID_UNDEF , ID_UNDEF , ID_SPTRDF, ID_INT   , ID_LLONG } },
      { 'd', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'd', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },
      { 'i', { ID_BYTE  , ID_SHORT , ID_INT   , ID_LONG  , ID_LLONG , ID_INTMAX, ID_SSIZET, ID_UNDEF , ID_UNDEF , ID_SPTRDF, ID_INT   , ID_LLONG } },
      { 'i', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'i', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },

      { 'o', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'o', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },
      { 'x', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'x', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },
      { 'X', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'X', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },
      { 'u', { ID_UBYTE , ID_USHORT, ID_UINT  , ID_ULONG , ID_ULLONG, ID_INTMAX, ID_SIZET , ID_UNDEF , ID_UNDEF , ID_PTRDIF, ID_UINT  , ID_ULLONG} },
      { 'u', { ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_ZERO  } },

      { 'f', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'f', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'f', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'f', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'f', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'F', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'F', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'F', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'F', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'F', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'e', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'e', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'e', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'e', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'e', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'E', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'E', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'E', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'E', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'E', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'a', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'a', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'a', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'a', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'a', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'A', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'A', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'A', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'A', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'A', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'g', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'g', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'g', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'g', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'g', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'G', { ID_UNDEF , ID_UNDEF , ID_DOUBLE, ID_DOUBLE, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_LNGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'G', { ID_UNDEF , ID_UNDEF , ID_NEGDBL, ID_NEGDBL, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NEGLNG, ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'G', { ID_UNDEF , ID_UNDEF , ID_INF   , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_INF   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'G', { ID_UNDEF , ID_UNDEF , ID_NAN   , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_NAN   , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'G', { ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_ZERO  , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },

    // boolalpha - not supported in snprintf per ISO C99 but is by boost::format so...
      { 'b', { ID_UNDEF , ID_UNDEF , ID_BOOLF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
      { 'b', { ID_UNDEF , ID_UNDEF , ID_BOOLT , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },

    // this is the terminator for conversion specifier loops:
      {  0 , { ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF , ID_UNDEF } },
};

std::string call_snprintf(const std::string& fmtStr, interop_datatype type)
{
    // enough space to hold any value in this test
    char buf[BUFSIZ];
    int len = 0;

    switch (type)
    {
        case ID_ZERO:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_z  ); break;
        case ID_BOOLF:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_bf ); break;
        case ID_BOOLT:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_bt ); break;
        case ID_BYTE:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_by ); break;
        case ID_UBYTE:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_uby); break;
        case ID_CHAR:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_c  ); break;
        case ID_WCHAR:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_wc ); break;
        case ID_STR:    len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_s  ); break;
        case ID_WSTR:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ws ); break;
        case ID_SHORT:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_h  ); break;
        case ID_USHORT: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_uh ); break;
        case ID_INT:    len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_i  ); break;
        case ID_UINT:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ui ); break;
        case ID_LONG:   len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_l  ); break;
        case ID_ULONG:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ul ); break;
        case ID_LLONG:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ll ); break;
        case ID_ULLONG: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ull); break;
        case ID_INTMAX: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_max); break;
        case ID_SSIZET: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_sst); break;
        case ID_SIZET:  len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_st ); break;
        case ID_SPTRDF: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_pt ); break;
        case ID_PTRDIF: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_pt ); break;
#if defined(INFINITY)
        case ID_INF:    len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), INFINITY); break;
#endif
#if defined(NAN)
        case ID_NAN:    len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), NAN);   break;
#endif
        case ID_DOUBLE: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_db ); break;
        case ID_NEGDBL: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ndb); break;
        case ID_LNGDBL: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_ldb); break;
        case ID_NEGLNG: len = SNPRINTF(buf, BUFSIZ, fmtStr.c_str(), g_nld); break;
        default: throw logic_error("unhandled type in call_snprintf");
    }

    if (len < 0)
    {
        throw logic_error("snprintf length 0");
    }

    return std::string(buf, len);
}

std::string call_format(const std::string& fmtStr, interop_datatype type)
{
    switch (type)
    {
        case ID_ZERO:   return ::boost::str(::boost::format(fmtStr) % g_z  );
        case ID_BOOLF:  return ::boost::str(::boost::format(fmtStr) % g_bf );
        case ID_BOOLT:  return ::boost::str(::boost::format(fmtStr) % g_bt );
        case ID_BYTE:   return ::boost::str(::boost::format(fmtStr) % g_by );
        case ID_UBYTE:  return ::boost::str(::boost::format(fmtStr) % g_uby);
        case ID_CHAR:   return ::boost::str(::boost::format(fmtStr) % g_c  );
        case ID_WCHAR:  return ::boost::str(::boost::format(fmtStr) % g_wc );
        case ID_STR:    return ::boost::str(::boost::format(fmtStr) % g_s  );
        case ID_WSTR:   return ::boost::str(::boost::format(fmtStr) % g_ws );
        case ID_SHORT:  return ::boost::str(::boost::format(fmtStr) % g_h  );
        case ID_USHORT: return ::boost::str(::boost::format(fmtStr) % g_uh );
        case ID_INT:    return ::boost::str(::boost::format(fmtStr) % g_i  );
        case ID_UINT:   return ::boost::str(::boost::format(fmtStr) % g_ui );
        case ID_LONG:   return ::boost::str(::boost::format(fmtStr) % g_l  );
        case ID_ULONG:  return ::boost::str(::boost::format(fmtStr) % g_ul );
        case ID_LLONG:  return ::boost::str(::boost::format(fmtStr) % g_ll );
        case ID_ULLONG: return ::boost::str(::boost::format(fmtStr) % g_ull);
        case ID_INTMAX: return ::boost::str(::boost::format(fmtStr) % g_max);
        case ID_SSIZET: return ::boost::str(::boost::format(fmtStr) % g_sst);
        case ID_SIZET:  return ::boost::str(::boost::format(fmtStr) % g_st );
        case ID_SPTRDF: return ::boost::str(::boost::format(fmtStr) % g_pt );
        case ID_PTRDIF: return ::boost::str(::boost::format(fmtStr) % g_pt );
#if defined(INFINITY)
        case ID_INF:    return ::boost::str(::boost::format(fmtStr) % INFINITY);
#endif
#if defined(NAN)
        case ID_NAN:    return ::boost::str(::boost::format(fmtStr) % NAN);
#endif
        case ID_DOUBLE: return ::boost::str(::boost::format(fmtStr) % g_db );
        case ID_NEGDBL: return ::boost::str(::boost::format(fmtStr) % g_ndb);
        case ID_LNGDBL: return ::boost::str(::boost::format(fmtStr) % g_ldb);
        case ID_NEGLNG: return ::boost::str(::boost::format(fmtStr) % g_nld);
        default: throw logic_error("unhandled type in call_format");
    }
}

po::variables_map g_args;
ofstream g_os;

void
write_header()
{
    if (g_args.count("snprintf"))
    {
#if BOOST_LIB_C_GNU
        g_os << "# glibc.version = " << BOOST_VERSION_NUMBER_MAJOR(BOOST_LIB_C_GNU) << "."
                                     << BOOST_VERSION_NUMBER_MINOR(BOOST_LIB_C_GNU) << "."
                                     << BOOST_VERSION_NUMBER_PATCH(BOOST_LIB_C_GNU)
                                     << endl;
#elif BOOST_COMP_MSVC
        g_os << "# msvc.version = "  << BOOST_VERSION_NUMBER_MAJOR(BOOST_COMP_MSVC) << "."
                                     << BOOST_VERSION_NUMBER_MINOR(BOOST_COMP_MSVC) << "."
                                     << BOOST_VERSION_NUMBER_PATCH(BOOST_COMP_MSVC)
                                     << endl;
#else
        g_os << "# libc.version = unknown" << endl;
#endif
    }
    else
    {
        g_os << "# boost.version = " << BOOST_VERSION / 100000 << "."      // major version
                                     << BOOST_VERSION / 100 % 1000 << "."  // minor version
                                     << BOOST_VERSION % 100                // patch level
                                     << endl;
    }
}

void
log(const std::string& spec, bool ok, const std::string& result)
{
    boost::io::ios_all_saver saver(g_os);
    g_os << setw(20) << right << spec << "\t"
         << (ok ? "OK " : "ERR") << "\t" << "\"" << result << "\"" << endl;
}

void cell(std::size_t nrow, std::size_t ncol)
{
    const interop_row& row(interop_matrix[nrow]);

    const interop_datatype& dataType(row.datatype[ncol]);
    if (dataType == ID_UNDEF || dataType == ID_NOTSUP)
    {
        return;
    }

#if !defined(INFINITY)
    if (dataType == ID_INF)
    {
        return;
    }
#endif

#if !defined(NAN)
    if (dataType == ID_NAN)
    {
        return;
    }
#endif

    // TODO: every combination of format flags - right now we do only one
    for (std::size_t ffi = 0; ffi < format_flags.size(); ++ffi)
    {
        for (std::size_t mwi = 0; mwi < minimum_width.size(); ++mwi)
        {
            for (std::size_t pri = 0; pri < precision.size(); ++pri)
            {
                // Make the format string
                std::stringstream fss;
                fss << '%';
                fss << format_flags[ffi];
                fss << minimum_width[mwi];
                fss << precision[pri];
                fss << length_modifier[ncol];
                fss << row.conversion_specifier;
                std::string fmtStr = fss.str();

                try
                {
                    std::string result = g_args.count("snprintf") ?
                                            call_snprintf(fmtStr, dataType) : 
                                            call_format  (fmtStr, dataType) ;
                    log(fmtStr, true, result);
                }
                catch (const std::exception& ex)
                {
                    log(fmtStr, false, ex.what());
                }
            }
        }
    }
}

void
matrix()
{
    for (std::size_t row = 0; interop_matrix[row].conversion_specifier != 0x00; ++row)
    {
        for (std::size_t col = 0; col < length_modifier.size(); ++col)
        {
            cell(row, col);
        }
    }
}

void
write_pctpct()
{
    if (g_args.count("snprintf"))
    {
        char buf[BUFSIZ];
        int len = SNPRINTF(buf, BUFSIZ, "%%");
        log("%%", len == 1, len == 1 ? buf : "snprintf length != 1");
    }
    else
    {
        try
        {
            log("%%", true, ::boost::format("%%").str());
        }
        catch (std::exception& ex)
        {
            log("%%", false, ex.what());
        }
    }
}

void
generate()
{
    string genpath = g_args["generate"].as<string>();
    g_os.open(genpath.c_str(), ios::out | ios::trunc);
    write_header();
    write_pctpct();
    matrix();
    g_os.close();
}

} // matrix

///////////////////////////////////////////////////////////////////////////////
//  main entry point
int
main(int argc, char *argv[])
{
    using matrix::g_args;

    po::options_description desc("Allowed options");
    desc.add_options()
        ("generate,g", po::value<string>()->required(), "generate output filename")
        ("help,h", "produce help message")
        ("snprintf,s", "use snprintf instead of boost::format")
        ;

    po::store(po::command_line_parser(argc, argv).options(desc).run(), g_args);
    po::notify(g_args);

    if (g_args.count("help")) {
        cout << "Usage: format_matrix [options]\n";
        cout << desc;
        return 0;
    }

    matrix::generate();

    return 0;
}
