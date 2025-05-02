// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_DETAIL_CONFIG_HPP
#define BOOST_PROCESS_V2_DETAIL_CONFIG_HPP

#if defined(BOOST_PROCESS_V2_STANDALONE)

#define BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(Sig) ASIO_COMPLETION_TOKEN_FOR(Sig)

#include <asio/detail/config.hpp>
#include <system_error>
#include <filesystem>
#include <string_view>
#include <iomanip>
#include <optional>

#if defined(ASIO_WINDOWS)
#define BOOST_PROCESS_V2_WINDOWS 1

#endif

#if defined(ASIO_HAS_UNISTD_H)
#define BOOST_PROCESS_V2_POSIX 1
#endif

#define BOOST_PROCESS_V2_BEGIN_NAMESPACE namespace process_v2 {
#define BOOST_PROCESS_V2_END_NAMESPACE   }
#define BOOST_PROCESS_V2_NAMESPACE process_v2

namespace asio {}
BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace net = ::asio;
BOOST_PROCESS_V2_END_NAMESPACE

#else

#define BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(Sig) BOOST_ASIO_COMPLETION_TOKEN_FOR(Sig)

#include <boost/config.hpp>
#include <boost/io/quoted.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_category.hpp>
#include <boost/system/system_error.hpp>
#include <boost/optional.hpp>

#if defined(BOOST_WINDOWS_API)
#define BOOST_PROCESS_V2_WINDOWS 1


#endif

#if defined(BOOST_POSIX_API)
#define BOOST_PROCESS_V2_POSIX 1
#endif

#if !defined(BOOST_PROCESS_V2_WINDOWS) && !defined(BOOST_POSIX_API)
#error Unsupported operating system
#endif

#if defined(BOOST_PROCESS_USE_STD_FS)
#include <filesystem>
#else
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#endif


#if !defined(BOOST_PROCESS_VERSION)
#define  BOOST_PROCESS_VERSION 2
#endif


#if BOOST_PROCESS_VERSION == 1
#define BOOST_PROCESS_V2_BEGIN_NAMESPACE namespace boost { namespace process { namespace v2 {
#else
#define BOOST_PROCESS_V2_BEGIN_NAMESPACE namespace boost { namespace process { inline namespace v2 {
#endif

#define BOOST_PROCESS_V2_END_NAMESPACE  } } }
#define BOOST_PROCESS_V2_NAMESPACE boost::process::v2

namespace boost { namespace asio {} }
BOOST_PROCESS_V2_BEGIN_NAMESPACE
namespace net = ::boost::asio;
BOOST_PROCESS_V2_END_NAMESPACE

#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(BOOST_PROCESS_STANDALONE)

using std::error_code ;
using std::error_category ;
using std::system_category ;
using std::system_error ;
namespace filesystem = std::filesystem;
using std::quoted;
using std::optional;

#define BOOST_PROCESS_V2_ASSIGN_EC(ec, ...) ec.assign(__VA_ARGS__);
#define BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec)                         \
  ec.assign(::BOOST_PROCESS_V2_NAMESPACE::detail::get_last_error());   \


#else

using boost::system::error_code ;
using boost::system::error_category ;
using boost::system::system_category ;
using boost::system::system_error ;
using boost::io::quoted;
using boost::optional;

#ifdef BOOST_PROCESS_USE_STD_FS
namespace filesystem = std::filesystem;
#else
namespace filesystem = boost::filesystem;
#endif

#define BOOST_PROCESS_V2_ASSIGN_EC(ec, ...)                       \
do                                                                \
{                                                                 \
  static constexpr auto loc##__LINE__((BOOST_CURRENT_LOCATION));  \
  ec.assign(__VA_ARGS__,  &loc##__LINE__);                        \
}                                                                 \
while (false)

#define BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec)                                         \
do                                                                                     \
{                                                                                      \
  static constexpr auto loc##__LINE__((BOOST_CURRENT_LOCATION));                       \
  ec.assign(::BOOST_PROCESS_V2_NAMESPACE::detail::get_last_error(), &loc##__LINE__);   \
}                                                                                      \
while (false)


#endif

BOOST_PROCESS_V2_END_NAMESPACE

#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_PROCESS_DYN_LINK)
#if defined(BOOST_PROCESS_SOURCE)
#define BOOST_PROCESS_V2_DECL BOOST_SYMBOL_EXPORT
#else
#define BOOST_PROCESS_V2_DECL BOOST_SYMBOL_IMPORT
#endif
#else
#define BOOST_PROCESS_V2_DECL
#endif

#if !defined(BOOST_PROCESS_SOURCE) && !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_PROCESS_NO_LIB)
#define BOOST_LIB_NAME boost_process
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_PROCESS_DYN_LINK)
#define BOOST_DYN_LINK
#endif
#include <boost/config/auto_link.hpp>
#endif 

#if defined(BOOST_PROCESS_V2_POSIX)

#if defined(__linux__) && !defined(BOOST_PROCESS_V2_DISABLE_PIDFD_OPEN)

#include <sys/syscall.h>

#if defined(SYS_pidfd_open)
#define BOOST_PROCESS_V2_PIDFD_OPEN 1
#define BOOST_PROCESS_V2_HAS_PROCESS_HANDLE 1
#endif
#endif

#if defined(__FreeBSD__) && defined(BOOST_PROCESS_V2_ENABLE_PDFORK)
#define BOOST_PROCESS_V2_PDFORK 1
#define BOOST_PROCESS_V2_HAS_PROCESS_HANDLE 1
#endif
#else
#define BOOST_PROCESS_V2_HAS_PROCESS_HANDLE 1
#endif

#endif //BOOST_PROCESS_V2_DETAIL_CONFIG_HPP
