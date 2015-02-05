//  filesystem unique_path.cpp  --------------------------------------------------------//

//  Copyright Beman Dawes 2010

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------// 

// define BOOST_FILESYSTEM_SOURCE so that <boost/filesystem/config.hpp> knows
// the library is being built (possibly exporting rather than importing code)
#define BOOST_FILESYSTEM_SOURCE 

#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/operations.hpp>

# ifdef BOOST_POSIX_API
#   include <fcntl.h>
# else // BOOST_WINDOWS_API
#   include <windows.h>
#   include <wincrypt.h>
#   pragma comment(lib, "Advapi32.lib")
# endif

namespace {

void fail(int err, boost::system::error_code* ec)
{
  if (ec == 0)
    BOOST_FILESYSTEM_THROW( boost::system::system_error(err,
      boost::system::system_category(),
      "boost::filesystem::unique_path"));

  ec->assign(err, boost::system::system_category());
  return;
}

void system_crypt_random(void* buf, std::size_t len, boost::system::error_code* ec)
{
# ifdef BOOST_POSIX_API

  int file = open("/dev/urandom", O_RDONLY);
  if (file == -1)
  {
    file = open("/dev/random", O_RDONLY);
    if (file == -1)
    {
      fail(errno, ec);
      return;
    }
  }

  size_t bytes_read = 0;
  while (bytes_read < len)
  {
    ssize_t n = read(file, buf, len - bytes_read);
    if (n == -1)
    {
      close(file);
      fail(errno, ec);
      return;
    }
    bytes_read += n;
    buf = static_cast<char*>(buf) + n;
  }

  close(file);

# else // BOOST_WINDOWS_API

  HCRYPTPROV handle;
  int errval = 0;

  if (!::CryptAcquireContextW(&handle, 0, 0, PROV_RSA_FULL, 0))
  {
    errval = ::GetLastError();
    if (errval == NTE_BAD_KEYSET)
    {
      if (!::CryptAcquireContextW(&handle, 0, 0, PROV_RSA_FULL, CRYPT_NEWKEYSET))
      {
        errval = ::GetLastError();
      }
      else errval = 0;
    }
  }

  if (!errval)
  {
    BOOL gen_ok = ::CryptGenRandom(handle, len, static_cast<unsigned char*>(buf));
    if (!gen_ok)
      errval = ::GetLastError();
    ::CryptReleaseContext(handle, 0);
  }

  if (!errval) return;

  fail(errval, ec);
# endif
}

}  // unnamed namespace

namespace boost { namespace filesystem { namespace detail {

BOOST_FILESYSTEM_DECL
path unique_path(const path& model, system::error_code* ec)
{
  std::wstring s (model.wstring());  // std::string ng for MBCS encoded POSIX
  const wchar_t hex[] = L"0123456789abcdef";
  const int n_ran = 16;
  const int max_nibbles = 2 * n_ran;   // 4-bits per nibble
  char ran[n_ran];

  int nibbles_used = max_nibbles;
  for(std::wstring::size_type i=0; i < s.size(); ++i)
  {
    if (s[i] == L'%')                        // digit request
    {
      if (nibbles_used == max_nibbles)
      {
        system_crypt_random(ran, sizeof(ran), ec);
        if (ec != 0 && *ec)
          return "";
        nibbles_used = 0;
      }
      int c = ran[nibbles_used/2];
      c >>= 4 * (nibbles_used++ & 1);  // if odd, shift right 1 nibble
      s[i] = hex[c & 0xf];             // convert to hex digit and replace
    }
  }

  if (ec != 0) ec->clear();

  return s;
}

}}}
