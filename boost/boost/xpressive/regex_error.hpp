///////////////////////////////////////////////////////////////////////////////
/// \file regex_error.hpp
/// Contains the definition of the regex_error exception class.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_REGEX_ERROR_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_REGEX_ERROR_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>
#include <stdexcept>
#include <boost/xpressive/regex_constants.hpp>

//{{AFX_DOC_COMMENT
///////////////////////////////////////////////////////////////////////////////
// This is a hack to get Doxygen to show the inheritance relation between
// regex_error and std::runtime_error.
#ifdef BOOST_XPRESSIVE_DOXYGEN_INVOKED
/// INTERNAL ONLY
namespace std
{
    /// INTERNAL ONLY
    struct runtime_error {};
}
#endif
//}}AFX_DOC_COMMENT

namespace boost { namespace xpressive
{

////////////////////////////////////////////////////////////////////////////////
//  regex_error
//
/// \brief The class regex_error defines the type of objects thrown as
/// exceptions to report errors during the conversion from a string representing
/// a regular expression to a finite state machine.
struct regex_error
  : std::runtime_error
{
    /// Constructs an object of class regex_error.
    /// \param code The error_type this regex_error represents.
    /// \post code() == code
    explicit regex_error(regex_constants::error_type code, char const *str = "")
      : std::runtime_error(str)
      , code_(code)
    {
    }

    /// Accessor for the error_type value
    /// \return the error_type code passed to the constructor
    /// \throw nothrow
    regex_constants::error_type code() const
    {
        return this->code_;
    }

private:

    regex_constants::error_type code_;
};

namespace detail
{

//////////////////////////////////////////////////////////////////////////
// ensure
/// INTERNAL ONLY
inline bool ensure(bool predicate, regex_constants::error_type code, char const *str = "")
{
    return predicate ? true : throw regex_error(code, str);
}

}}} // namespace boost::xpressive::detail

#endif
