//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ENCODING_UTF_HPP_INCLUDED
#define BOOST_LOCALE_ENCODING_UTF_HPP_INCLUDED

#include <boost/locale/detail/allocator_traits.hpp>
#include <boost/locale/encoding_errors.hpp>
#include <boost/locale/utf.hpp>
#include <boost/locale/util/string.hpp>
#include <iterator>
#include <memory>
#include <type_traits>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale { namespace conv {
    /// \addtogroup codepage
    ///
    /// @{

    /// Convert a Unicode text in range [begin,end) to other Unicode encoding
    ///
    /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be decoded)
    template<typename CharOut, typename CharIn, class Alloc = std::allocator<CharOut>>
    std::basic_string<CharOut, std::char_traits<CharOut>, Alloc>
    utf_to_utf(const CharIn* begin, const CharIn* end, method_type how = default_method, const Alloc& alloc = Alloc())
    {
        std::basic_string<CharOut, std::char_traits<CharOut>, Alloc> result(alloc);
        result.reserve(end - begin);
        auto inserter = std::back_inserter(result);
        while(begin != end) {
            const utf::code_point c = utf::utf_traits<CharIn>::decode(begin, end);
            if(c == utf::illegal || c == utf::incomplete) {
                if(how == stop)
                    throw conversion_error();
            } else
                utf::utf_traits<CharOut>::encode(c, inserter);
        }
        return result;
    }

    /// Convert a Unicode string \a str to other Unicode encoding.
    /// Invalid characters are skipped.
    template<typename CharOut, typename CharIn, class Alloc>
    std::basic_string<CharOut, std::char_traits<CharOut>, Alloc>
    utf_to_utf(const CharIn* begin, const CharIn* end, const Alloc& alloc)
    {
        return utf_to_utf<CharOut>(begin, end, skip, alloc);
    }

    /// Convert a Unicode NULL terminated string \a str to other Unicode encoding
    ///
    /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be decoded)
    template<typename CharOut, typename CharIn, class Alloc = std::allocator<CharOut>>
    std::basic_string<CharOut, std::char_traits<CharOut>, Alloc>
    utf_to_utf(const CharIn* str, method_type how = default_method, const Alloc& alloc = Alloc())
    {
        return utf_to_utf<CharOut>(str, util::str_end(str), how, alloc);
    }

    /// Convert a Unicode string \a str to other Unicode encoding.
    /// Invalid characters are skipped.
    template<typename CharOut, typename CharIn, class Alloc>
#ifndef BOOST_LOCALE_DOXYGEN
    detail::enable_if_allocator_for<Alloc,
                                    CharOut,
#endif
                                    std::basic_string<CharOut, std::char_traits<CharOut>, Alloc>
#ifndef BOOST_LOCALE_DOXYGEN
                                    >
#endif
    utf_to_utf(const CharIn* str, const Alloc& alloc)
    {
        return utf_to_utf<CharOut>(str, skip, alloc);
    }

    /// Convert a Unicode string \a str to other Unicode encoding
    ///
    /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be decoded)
    template<typename CharOut, typename CharIn, class Alloc>
#ifndef BOOST_LOCALE_DOXYGEN
    detail::enable_if_allocator_for<
      Alloc,
      CharIn,
#endif
      std::basic_string<CharOut, std::char_traits<CharOut>, detail::rebind_alloc<Alloc, CharOut>>
#ifndef BOOST_LOCALE_DOXYGEN
      >
#endif
    utf_to_utf(const std::basic_string<CharIn, std::char_traits<CharIn>, Alloc>& str, method_type how = default_method)
    {
        return utf_to_utf<CharOut>(str.c_str(),
                                   str.c_str() + str.size(),
                                   how,
                                   detail::rebind_alloc<Alloc, CharOut>(str.get_allocator()));
    }

    /// Convert a Unicode string \a str to other Unicode encoding
    ///
    /// \throws conversion_error: Conversion failed (e.g. \a how is \c stop and any character cannot be decoded)
    template<typename CharOut, typename CharIn, class AllocOut, class AllocIn>
#ifndef BOOST_LOCALE_DOXYGEN
    detail::enable_if_allocator_for<AllocIn,
                                    CharIn,
#endif
                                    std::basic_string<CharOut, std::char_traits<CharOut>, AllocOut>
#ifndef BOOST_LOCALE_DOXYGEN
                                    >
#endif
    utf_to_utf(const std::basic_string<CharIn, std::char_traits<CharIn>, AllocIn>& str,
               method_type how = default_method,
               const AllocOut& alloc = AllocOut())
    {
        return utf_to_utf<CharOut>(str.c_str(), str.c_str() + str.size(), how, alloc);
    }

    /// Convert a Unicode string \a str to other Unicode encoding.
    /// Invalid characters are skipped.
    template<typename CharOut, typename CharIn, class AllocOut, class AllocIn>
#ifndef BOOST_LOCALE_DOXYGEN
    detail::enable_if_allocator_for2<AllocIn,
                                     CharIn,
                                     AllocOut,
                                     CharOut,
#endif
                                     std::basic_string<CharOut, std::char_traits<CharOut>, AllocOut>
#ifndef BOOST_LOCALE_DOXYGEN
                                     >
#endif
    utf_to_utf(const std::basic_string<CharIn, std::char_traits<CharIn>, AllocIn>& str, const AllocOut& alloc)
    {
        return utf_to_utf<CharOut>(str, skip, alloc);
    }

    /// @}

}}} // namespace boost::locale::conv

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
