//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ENCODING_ERRORS_HPP_INCLUDED
#define BOOST_LOCALE_ENCODING_ERRORS_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <stdexcept>
#include <string>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale { namespace conv {
    /// \addtogroup codepage
    ///
    /// @{

    /// \brief The exception that is thrown in case of conversion error
    class BOOST_SYMBOL_VISIBLE conversion_error : public std::runtime_error {
    public:
        conversion_error() : std::runtime_error("Conversion failed") {}
    };

    /// \brief This exception is thrown in case of use of unsupported
    /// or invalid character set
    class BOOST_SYMBOL_VISIBLE invalid_charset_error : public std::runtime_error {
    public:
        /// Create an error for charset \a charset
        invalid_charset_error(const std::string& charset) :
            std::runtime_error("Invalid or unsupported charset: " + charset)
        {}
    };

    /// enum that defines conversion policy
    enum method_type {
        skip = 0,             ///< Skip illegal/unconvertible characters
        stop = 1,             ///< Stop conversion and throw conversion_error
        default_method = skip ///< Default method - skip
    };

    /// @}

}}} // namespace boost::locale::conv

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
