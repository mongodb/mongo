/* boost random_device.cpp implementation
 *
 * Copyright Jens Maurer 2000
 * Copyright Steven Watanabe 2010-2011
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * $Id$
 *
 */

#define BOOST_RANDOM_SOURCE

#include <boost/random/random_device.hpp>
#include <boost/config.hpp>
#include <boost/throw_exception.hpp>
#include <boost/assert.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/system/system_error.hpp>
#include <boost/system/error_code.hpp>
#include <string>

#if !defined(BOOST_NO_INCLASS_MEMBER_INITIALIZATION) && !BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1600))
//  A definition is required even for integral static constants
const bool boost::random::random_device::has_fixed_range;
#endif

// WinRT target.
#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
# if defined(__cplusplus_winrt)
#  include <winapifamily.h>
#  if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
#   define BOOST_RANDOM_WINDOWS_RUNTIME 1
#  endif
# endif
#endif

#if defined(BOOST_WINDOWS)

#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
#include <windows.h>
#include <wincrypt.h>
#include <stdexcept>  // std::invalid_argument
#else
using namespace Platform;
using namespace Windows::Security::Cryptography;
#endif

#define BOOST_AUTO_LINK_NOMANGLE
#define BOOST_LIB_NAME "Advapi32"
#include <boost/config/auto_link.hpp>

#ifdef __MINGW32__

extern "C" {

// mingw's wincrypt.h appears to be missing some things
WINADVAPI
BOOL
WINAPI
CryptEnumProvidersA(
    DWORD dwIndex,
    DWORD *pdwReserved,
    DWORD dwFlags,
    DWORD *pdwProvType,
    LPSTR szProvName,
    DWORD *pcbProvName
    );

}

#endif

namespace {
#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
const char * const default_token = MS_DEF_PROV_A;
#else
const char * const default_token = "";
#endif
}

class boost::random::random_device::impl
{
public:
  impl(const std::string & token) : provider(token) {
#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
    char buffer[80];
    DWORD type;
    DWORD len;

    // Find the type of a specific provider
    for(DWORD i = 0; ; ++i) {
      len = sizeof(buffer);
      if(!CryptEnumProvidersA(i, NULL, 0, &type, buffer, &len)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
        continue;
      }
      if(buffer == provider) {
        break;
      }
    }

    if(!CryptAcquireContextA(&hProv, NULL, provider.c_str(), type,
        CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
      error("Could not acquire CSP context");
    }
#endif
  }

#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
  ~impl() {
    if(!CryptReleaseContext(hProv, 0)) error("Could not release CSP context");
  }
#endif

  unsigned int next() {
    unsigned int result;

#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
    if(!CryptGenRandom(hProv, sizeof(result),
        static_cast<BYTE*>(static_cast<void*>(&result)))) {
      error("error while reading");
    }
#else
    auto buffer = CryptographicBuffer::GenerateRandom(sizeof(result));
    auto data = ref new Array<unsigned char>(buffer->Length);
    CryptographicBuffer::CopyToByteArray(buffer, &data);
    memcpy(&result, data->begin(), data->end() - data->begin());
#endif

    return result;
  }

private:
#if !defined(BOOST_RANDOM_WINDOWS_RUNTIME)
  void error(const char * msg) {
    DWORD error_code = GetLastError();
    boost::throw_exception(
      boost::system::system_error(
        error_code, boost::system::system_category(),
        std::string("boost::random_device: ") + msg + 
        " Cryptographic Service Provider " + provider));
  }
  HCRYPTPROV hProv;
#endif
  const std::string provider;
};

#else

namespace {
// the default is the unlimited capacity device, using some secure hash
// try "/dev/random" for blocking when the entropy pool has drained
const char * const default_token = "/dev/urandom";
}

/*
 * This uses the POSIX interface for unbuffered reading.
 * Using buffered std::istream would consume entropy which may
 * not actually be used.  Entropy is a precious good we avoid
 * wasting.
 */

#if defined(__GNUC__) && defined(_CXXRT_STD_NAME)
// I have severe difficulty to get the POSIX includes to work with
// -fhonor-std and Dietmar Kuhl's standard C++ library.  Hack around that
// problem for now.
extern "C" {
static const int O_RDONLY = 0;
extern int open(const char *__file, int __oflag, ...);
extern int read(int __fd, __ptr_t __buf, size_t __nbytes);
extern int close(int __fd);
}
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>    // open
#include <unistd.h>   // read, close
#endif

#include <errno.h>    // errno
#include <string.h>   // strerror
#include <stdexcept>  // std::invalid_argument


class boost::random::random_device::impl
{
public:
  impl(const std::string & token) : path(token) {
    fd = open(token.c_str(), O_RDONLY);
    if(fd < 0)
      error("cannot open");
  }

  ~impl() { if(close(fd) < 0) error("could not close"); }

  unsigned int next() {
    unsigned int result;
    long sz = read(fd, reinterpret_cast<char *>(&result), sizeof(result));
    if(sz == -1)
      error("error while reading");
    else if(sz != sizeof(result)) {
      errno = 0;
      error("EOF while reading");
    }
    return result;
  }

private:
  void error(const char * msg) {
    int error_code = errno;
    boost::throw_exception(
      boost::system::system_error(
        error_code, boost::system::system_category(),
        std::string("boost::random_device: ") + msg + 
        " random-number pseudo-device " + path));
  }
  const std::string path;
  int fd;
};

#endif // BOOST_WINDOWS

BOOST_RANDOM_DECL boost::random::random_device::random_device()
  : pimpl(new impl(default_token))
{}

BOOST_RANDOM_DECL boost::random::random_device::random_device(const std::string& token)
  : pimpl(new impl(token))
{}

BOOST_RANDOM_DECL boost::random_device::~random_device()
{
  delete pimpl;
}

BOOST_RANDOM_DECL double boost::random_device::entropy() const
{
  return 10;
}

BOOST_RANDOM_DECL unsigned int boost::random_device::operator()()
{
  return pimpl->next();
}
