// Copyright Vladimir Prus 2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/config.hpp>

#ifdef BOOST_NO_STD_WSTREAMBUF
#error "wide char i/o not supported on this platform"
#endif

// include boost implementation of utf8 codecvt facet
# define BOOST_ARCHIVE_SOURCE
#include <boost/archive/detail/decl.hpp>
#define BOOST_UTF8_BEGIN_NAMESPACE \
     namespace boost { namespace archive { namespace detail {
#define BOOST_UTF8_DECL BOOST_ARCHIVE_DECL
#define BOOST_UTF8_END_NAMESPACE }}}
#include <boost/detail/utf8_codecvt_facet.ipp>
#undef BOOST_UTF8_END_NAMESPACE
#undef BOOST_UTF8_DECL
#undef BOOST_UTF8_BEGIN_NAMESPACE
