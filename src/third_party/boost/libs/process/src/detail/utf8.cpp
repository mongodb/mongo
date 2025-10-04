// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/utf8.hpp>
#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/error.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace detail
{

#if defined(BOOST_PROCESS_V2_WINDOWS)

inline void handle_error(error_code & ec)
{
    const auto err = ::GetLastError();
    switch (err)
    {
    case ERROR_INSUFFICIENT_BUFFER:
        BOOST_PROCESS_V2_ASSIGN_EC(ec, error::insufficient_buffer, error::utf8_category);
        break;
    case ERROR_NO_UNICODE_TRANSLATION:
        BOOST_PROCESS_V2_ASSIGN_EC(ec, error::invalid_character, error::utf8_category);
        break;
    default:
        BOOST_PROCESS_V2_ASSIGN_EC(ec, err, system_category());
    }
}

std::size_t size_as_utf8(const wchar_t * in, std::size_t size, error_code & ec)
{
    auto res = WideCharToMultiByte(
                          CP_UTF8,                // CodePage,
                          0,                      // dwFlags,
                          in,                     // lpWideCharStr,
                          static_cast<int>(size), // cchWideChar,
                          nullptr,                // lpMultiByteStr,
                          0,                      // cbMultiByte,
                          nullptr,                // lpDefaultChar,
                          FALSE);                 // lpUsedDefaultChar
    
    if (res == 0u)
        handle_error(ec);
    return static_cast<std::size_t>(res);
}

std::size_t size_as_wide(const char * in, std::size_t size, error_code & ec)
{
    auto res = ::MultiByteToWideChar(
                          CP_UTF8,                // CodePage
                          0,                      // dwFlags
                          in,                     // lpMultiByteStr
                          static_cast<int>(size), // cbMultiByte
                          nullptr,                // lpWideCharStr
                          0);                     // cchWideChar
    if (res == 0u)
        handle_error(ec);

    return static_cast<std::size_t>(res);
}

std::size_t convert_to_utf8(const wchar_t *in, std::size_t size,  char * out, 
                            std::size_t max_size, error_code & ec)
{
    auto res = ::WideCharToMultiByte(
                    CP_UTF8,                    // CodePage
                    0,                          // dwFlags
                    in,                         // lpWideCharStr
                    static_cast<int>(size),     // cchWideChar
                    out,                        // lpMultiByteStr
                    static_cast<int>(max_size), // cbMultiByte
                    nullptr,                    // lpDefaultChar
                    FALSE);                     // lpUsedDefaultChar
    if (res == 0u)
        handle_error(ec);

    return static_cast<std::size_t>(res);
}

std::size_t convert_to_wide(const char *in, std::size_t size,  wchar_t * out, 
                            std::size_t max_size, error_code & ec)
{
    auto res = ::MultiByteToWideChar(
                          CP_UTF8,                     // CodePage
                          0,                           // dwFlags
                          in,                          // lpMultiByteStr
                          static_cast<int>(size),      // cbMultiByte
                          out,                         // lpWideCharStr
                          static_cast<int>(max_size)); // cchWideChar
    if (res == 0u)
        handle_error(ec);

    return static_cast<std::size_t>(res);
}

#else


template<std::size_t s>
inline int get_cont_octet_out_count_impl(wchar_t word) {
    if (word < 0x80) {
        return 0;
    }
    if (word < 0x800) {
        return 1;
    }
    return 2;
}

template<>
inline int get_cont_octet_out_count_impl<4>(wchar_t word) {
    if (word < 0x80) {
        return 0;
    }
    if (word < 0x800) {
        return 1;
    }

    // Note that the following code will generate warnings on some platforms
    // where wchar_t is defined as UCS2.  The warnings are superfluous as the
    // specialization is never instantiated with such compilers, but this
    // can cause problems if warnings are being treated as errors, so we guard
    // against that. Including <boost/detail/utf8_codecvt_facet.hpp> as we do
    // should be enough to get WCHAR_MAX defined.
#if !defined(WCHAR_MAX)
#   error WCHAR_MAX not defined!
#endif
    // cope with VC++ 7.1 or earlier having invalid WCHAR_MAX
#if defined(_MSC_VER) && _MSC_VER <= 1310 // 7.1 or earlier
    return 2;
#elif WCHAR_MAX > 0x10000

    if (word < 0x10000) {
        return 2;
    }
    if (word < 0x200000) {
        return 3;
    }
    if (word < 0x4000000) {
        return 4;
    }
    return 5;

#else
    return 2;
#endif
}

inline int get_cont_octet_out_count(wchar_t word)
{
    return detail::get_cont_octet_out_count_impl<sizeof(wchar_t)>(word);
}

// copied from boost/detail/utf8_codecvt_facet.ipp
// Copyright (c) 2001 Ronald Garcia, Indiana University (garcia@osl.iu.edu)
// Andrew Lumsdaine, Indiana University (lums@osl.iu.edu).

inline unsigned int get_octet_count(unsigned char lead_octet)
{
    // if the 0-bit (MSB) is 0, then 1 character
    if (lead_octet <= 0x7f) return 1;

    // Otherwise the count number of consecutive 1 bits starting at MSB
//    assert(0xc0 <= lead_octet && lead_octet <= 0xfd);

    if (0xc0 <= lead_octet && lead_octet <= 0xdf) return 2;
    else if (0xe0 <= lead_octet && lead_octet <= 0xef) return 3;
    else if (0xf0 <= lead_octet && lead_octet <= 0xf7) return 4;
    else if (0xf8 <= lead_octet && lead_octet <= 0xfb) return 5;
    else return 6;
}

inline bool invalid_continuing_octet(unsigned char octet_1) {
    return (octet_1 < 0x80|| 0xbf< octet_1);
}

inline unsigned int get_cont_octet_count(unsigned char lead_octet)
{
    return get_octet_count(lead_octet) - 1;
}

inline const wchar_t * get_octet1_modifier_table() noexcept
{
    static const wchar_t octet1_modifier_table[] = {
        0x00, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc
    };
    return octet1_modifier_table;
}


std::size_t size_as_utf8(const wchar_t * in, std::size_t size, error_code &)
{
    std::size_t res = 0u;
    const auto from_end = in + size;
    for (auto from = in; from != from_end; from++)
        res += get_cont_octet_out_count(*from) + 1;
    return res;
}

std::size_t size_as_wide(const  char   * in, std::size_t size, error_code &)
{
    const auto from = in;
    const auto from_end = from + size;
    const char * from_next = from;
    std::size_t char_count = 0u;
    while (from_next < from_end)
    {
        ++char_count;
        unsigned int octet_count = get_octet_count(*from_next);
        // The buffer may represent incomplete characters, so terminate early if one is found
        if (octet_count > static_cast<std::size_t>(from_end - from_next))
            break;
        from_next += octet_count;
    }

    return from_next - from;
}

std::size_t convert_to_utf8(const wchar_t * in, std::size_t size,
                            char   * out, std::size_t max_size, error_code & ec)
{

    const wchar_t * from = in;
    const wchar_t * from_end = from + size;
    const wchar_t * & from_next = from;
    char * to = out;
    char * to_end = out + max_size;
    char * & to_next = to;

    const wchar_t * const octet1_modifier_table = get_octet1_modifier_table();
    wchar_t max_wchar = (std::numeric_limits<wchar_t>::max)();
    while (from != from_end && to != to_end) {

        // Check for invalid UCS-4 character
        if (*from  > max_wchar) {
            from_next = from;
            to_next = to;
            BOOST_PROCESS_V2_ASSIGN_EC(ec, error::invalid_character, error::get_utf8_category());
            return 0u;
        }

        int cont_octet_count = get_cont_octet_out_count(*from);

        // RG - comment this formula better
        int shift_exponent = cont_octet_count * 6;

        // Process the first character
        *to++ = static_cast<char>(octet1_modifier_table[cont_octet_count] +
                                  (unsigned char)(*from / (1 << shift_exponent)));

        // Process the continuation characters
        // Invariants: At the start of the loop:
        //   1) 'i' continuing octets have been generated
        //   2) '*to' points to the next location to place an octet
        //   3) shift_exponent is 6 more than needed for the next octet
        int i = 0;
        while (i != cont_octet_count && to != to_end) {
            shift_exponent -= 6;
            *to++ = static_cast<char>(0x80 + ((*from / (1 << shift_exponent)) % (1 << 6)));
            ++i;
        }
        // If we filled up the out buffer before encoding the character
        if (to == to_end && i != cont_octet_count) {
            from_next = from;
            to_next = to - (i + 1);
            BOOST_PROCESS_V2_ASSIGN_EC(ec, error::insufficient_buffer, error::get_utf8_category());
            return 0u;
        }
        ++from;
    }
    from_next = from;
    to_next = to;

    // Were we done or did we run out of destination space
    if (from != from_end)
        BOOST_PROCESS_V2_ASSIGN_EC(ec, error::insufficient_buffer, error::get_utf8_category());

    return to_next - out;
}

inline bool invalid_leading_octet(unsigned char octet_1) {
    return (0x7f < octet_1 && octet_1 < 0xc0) ||
           (octet_1 > 0xfd);
}

std::size_t convert_to_wide(const  char   * in, std::size_t size,
                            wchar_t * out, std::size_t max_size, error_code & ec)
{
    const char * from = in;
    const char * from_end = from + size;
    const char * & from_next = from;
    wchar_t * to = out;
    wchar_t * to_end = out + max_size;
    wchar_t * & to_next = to;

    // Basic algorithm: The first octet determines how many
    // octets total make up the UCS-4 character. The remaining
    // "continuing octets" all begin with "10". To convert, subtract
    // the amount that specifies the number of octets from the first
    // octet. Subtract 0x80 (1000 0000) from each continuing octet,
    // then mash the whole lot together. Note that each continuing
    // octet only uses 6 bits as unique values, so only shift by
    // multiples of 6 to combine.
    const wchar_t * const octet1_modifier_table = detail::get_octet1_modifier_table();
    while (from != from_end && to != to_end) {

        // Error checking on the first octet
        if (invalid_leading_octet(*from)) {
            from_next = from;
            to_next = to;
            BOOST_PROCESS_V2_ASSIGN_EC(ec, error::invalid_character, error::get_utf8_category());
            return 0u;
        }

        // The first octet is adjusted by a value dependent upon
        // the number of "continuing octets" encoding the character
        const int cont_octet_count = get_cont_octet_count(*from);

        // The unsigned char conversion is necessary in case char is
        // signed (I learned this the hard way)
        wchar_t ucs_result =
                (unsigned char)(*from++) - octet1_modifier_table[cont_octet_count];

        // Invariants:
        //   1) At the start of the loop, 'i' continuing characters have been
        //      processed
        //   2) *from points to the next continuing character to be processed.
        int i = 0;
        while (i != cont_octet_count && from != from_end) {

            // Error checking on continuing characters
            if (invalid_continuing_octet(*from)) {
                from_next = from;
                to_next = to;
                BOOST_PROCESS_V2_ASSIGN_EC(ec, error::invalid_character, error::get_utf8_category());
                return 0u;
            }

            ucs_result *= (1 << 6);

            // each continuing character has an extra (10xxxxxx)b attached to
            // it that must be removed.
            ucs_result += (unsigned char)(*from++) - 0x80;
            ++i;
        }

        // If the buffer ends with an incomplete unicode character...
        if (from == from_end && i != cont_octet_count) {
            // rewind "from" to before the current character translation
            from_next = from - (i + 1);
            to_next = to;
            BOOST_PROCESS_V2_ASSIGN_EC(ec, error::insufficient_buffer, error::get_utf8_category());
            return 0u;
        }
        *to++ = ucs_result;
    }
    from_next = from;
    to_next = to;

    if (from != from_end)
        BOOST_PROCESS_V2_ASSIGN_EC(ec, error::insufficient_buffer, error::get_utf8_category());

    return to_next - out;
}

#endif

}

BOOST_PROCESS_V2_END_NAMESPACE

