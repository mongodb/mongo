//  operations.cpp  ----------------------------------------------------------//

//  Copyright 2002-2005 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy
//  at http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------// 

// define BOOST_FILESYSTEM_SOURCE so that <boost/filesystem/config.hpp> knows
// the library is being built (possibly exporting rather than importing code)
#define BOOST_FILESYSTEM_SOURCE

#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#define _POSIX_PTHREAD_SEMANTICS  // Sun readdir_r() needs this

#if !(defined(__HP_aCC) && defined(_ILP32) && \
      !defined(_STATVFS_ACPP_PROBLEMS_FIXED))
#define _FILE_OFFSET_BITS 64 // at worst, these defines may have no effect,
#endif
#if !defined(__PGI)
#define __USE_FILE_OFFSET64 // but that is harmless on Windows and on POSIX
      // 64-bit systems or on 32-bit systems which don't have files larger 
      // than can be represented by a traditional POSIX/UNIX off_t type. 
      // OTOH, defining them should kick in 64-bit off_t's (and thus 
      // st_size) on 32-bit systems that provide the Large File
      // Support (LFS) interface, such as Linux, Solaris, and IRIX.
      // The defines are given before any headers are included to
      // ensure that they are available to all included headers.
      // That is required at least on Solaris, and possibly on other
      // systems as well.

// for some compilers (CodeWarrior, for example), windows.h
// is getting included by some other boost header, so do this early:
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0500 // Default to Windows 2K or later
#endif
#endif


#include <boost/filesystem/v2/operations.hpp>
#include <boost/scoped_array.hpp>
#include <boost/assert.hpp>
#include <boost/detail/workaround.hpp>
#include <cstdlib>  // for malloc, free

namespace fs = boost::filesystem2;
using boost::system::error_code;
using boost::system::system_category;

# if defined(BOOST_WINDOWS_API)
#   include <windows.h>
#   include <ctime>  // for time_t

# else // BOOST_POSIX_API
#   include <sys/types.h>
#   if !defined(__APPLE__) && !defined(__OpenBSD__)
#     include <sys/statvfs.h>
#     define BOOST_STATVFS statvfs
#     define BOOST_STATVFS_F_FRSIZE vfs.f_frsize
#   else
#ifdef __OpenBSD__
#     include <sys/param.h>
#endif
#     include <sys/mount.h>
#     define BOOST_STATVFS statfs
#     define BOOST_STATVFS_F_FRSIZE static_cast<boost::uintmax_t>( vfs.f_bsize )
#   endif
#   include <dirent.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <utime.h>
#   include "limits.h"
# endif

//  BOOST_FILESYSTEM_STATUS_CACHE enables file_status cache in
//  dir_itr_increment. The config tests are placed here because some of the
//  macros being tested come from dirent.h.
//
// TODO: find out what macros indicate dirent::d_type present in more libraries
# if defined(BOOST_WINDOWS_API) \
  || (defined(_DIRENT_HAVE_D_TYPE) /* defined by GNU C library if d_type present */ \
    && !(defined(__SUNPRO_CC) && !defined(__sun)))  // _DIRENT_HAVE_D_TYPE wrong for Sun compiler on Linux
#   define BOOST_FILESYSTEM_STATUS_CACHE
# endif

#include <sys/stat.h>  // even on Windows some functions use stat()
#include <string>
#include <cstring>
#include <cstdio>      // for remove, rename
#include <cerrno>
// #include <iostream>    // for debugging only; comment out when not in use

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::strcmp; using ::remove; using ::rename; }
#endif

//  helpers  -----------------------------------------------------------------//

namespace
{
  const error_code ok;

  bool is_empty_directory( const std::string & dir_path )
  {
    static const fs::directory_iterator end_itr;
    return fs::directory_iterator(fs::path(dir_path)) == end_itr;
  }

#ifdef BOOST_WINDOWS_API
  
// For Windows, the xxxA form of various function names is used to avoid
// inadvertently getting wide forms of the functions. (The undecorated
// forms are actually macros, so can misfire if the user has various
// other macros defined. There was a bug report of this happening.)

  inline DWORD get_file_attributes( const char * ph )
    { return ::GetFileAttributesA( ph ); }

# ifndef BOOST_FILESYSTEM2_NARROW_ONLY

  inline DWORD get_file_attributes( const wchar_t * ph )
    { return ::GetFileAttributesW( ph ); }

  bool is_empty_directory( const std::wstring & dir_path )
  {
    static const fs::wdirectory_iterator wend_itr;
    return fs::wdirectory_iterator(fs::wpath(dir_path)) == wend_itr;
  }

  inline BOOL get_file_attributes_ex( const wchar_t * ph,
    WIN32_FILE_ATTRIBUTE_DATA & fad )
  { return ::GetFileAttributesExW( ph, ::GetFileExInfoStandard, &fad ); }
      
  HANDLE create_file( const wchar_t * ph, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile )
  {
    return ::CreateFileW( ph, dwDesiredAccess, dwShareMode,
      lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
      hTemplateFile );
  }

  inline DWORD get_current_directory( DWORD sz, wchar_t * buf )
    { return ::GetCurrentDirectoryW( sz, buf ); } 

  inline bool set_current_directory( const wchar_t * buf )
    { return ::SetCurrentDirectoryW( buf ) != 0 ; } 

  inline bool get_free_disk_space( const std::wstring & ph,
    PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free )
    { return ::GetDiskFreeSpaceExW( ph.c_str(), avail, total, free ) != 0; }

  inline std::size_t get_full_path_name(
    const std::wstring & ph, std::size_t len, wchar_t * buf, wchar_t ** p )
  {
    return static_cast<std::size_t>(
      ::GetFullPathNameW( ph.c_str(),
        static_cast<DWORD>(len), buf, p ));
  } 

  inline bool remove_directory( const std::wstring & ph )
    { return ::RemoveDirectoryW( ph.c_str() ) != 0; }

  inline bool delete_file( const std::wstring & ph )
    { return ::DeleteFileW( ph.c_str() ) != 0; }

  inline bool create_directory( const std::wstring & dir )
    {  return ::CreateDirectoryW( dir.c_str(), 0 ) != 0; }

#if _WIN32_WINNT >= 0x500
  inline bool create_hard_link( const std::wstring & to_ph,
    const std::wstring & from_ph )
    {  return ::CreateHardLinkW( from_ph.c_str(), to_ph.c_str(), 0 ) != 0; }
#endif
  
# endif // ifndef BOOST_FILESYSTEM2_NARROW_ONLY

  template< class String >
  fs::file_status status_template( const String & ph, error_code & ec )
  {
    DWORD attr( get_file_attributes( ph.c_str() ) );
    if ( attr == 0xFFFFFFFF )
    {
      ec = error_code( ::GetLastError(), system_category() );
      if ((ec.value() == ERROR_FILE_NOT_FOUND)
        || (ec.value() == ERROR_PATH_NOT_FOUND)
        || (ec.value() == ERROR_INVALID_NAME) // "tools/jam/src/:sys:stat.h", "//foo"
        || (ec.value() == ERROR_INVALID_DRIVE) // USB card reader with no card inserted
        || (ec.value() == ERROR_NOT_READY) // CD/DVD drive with no disc inserted
        || (ec.value() == ERROR_INVALID_PARAMETER) // ":sys:stat.h"
        || (ec.value() == ERROR_BAD_PATHNAME) // "//nosuch" on Win64
        || (ec.value() == ERROR_BAD_NETPATH)) // "//nosuch" on Win32
      {
        ec = ok; // these are not considered errors;
                           // the status is considered not found
        return fs::file_status( fs::file_not_found );
      }
      else if ((ec.value() == ERROR_SHARING_VIOLATION))
      {
        ec = ok; // these are not considered errors;
                           // the file exists but the type is not known 
        return fs::file_status( fs::type_unknown );
      }
      return fs::file_status( fs::status_unknown );
    }
    ec = ok;;
    return (attr & FILE_ATTRIBUTE_DIRECTORY)
      ? fs::file_status( fs::directory_file )
      : fs::file_status( fs::regular_file );
  }

  BOOL get_file_attributes_ex( const char * ph,
    WIN32_FILE_ATTRIBUTE_DATA & fad )
  { return ::GetFileAttributesExA( ph, ::GetFileExInfoStandard, &fad ); }

  template< class String >
  boost::filesystem2::detail::query_pair
  is_empty_template( const String & ph )
  {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if ( get_file_attributes_ex( ph.c_str(), fad ) == 0 )
      return std::make_pair( error_code( ::GetLastError(), system_category() ), false );    
    return std::make_pair( ok,
      ( fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
        ? is_empty_directory( ph )
        :( !fad.nFileSizeHigh && !fad.nFileSizeLow ) );
  }

  HANDLE create_file( const char * ph, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile )
  {
    return ::CreateFileA( ph, dwDesiredAccess, dwShareMode,
      lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
      hTemplateFile );
  }

  // Thanks to Jeremy Maitin-Shepard for much help and for permission to
  // base the equivalent() implementation on portions of his 
  // file-equivalence-win32.cpp experimental code.
  struct handle_wrapper
  {
    HANDLE handle;
    handle_wrapper( HANDLE h )
      : handle(h) {}
    ~handle_wrapper()
    {
      if ( handle != INVALID_HANDLE_VALUE )
        ::CloseHandle(handle);
    }
  };

  template< class String >
  boost::filesystem2::detail::query_pair
  equivalent_template( const String & ph1, const String & ph2 )
  {
    // Note well: Physical location on external media is part of the
    // equivalence criteria. If there are no open handles, physical location
    // can change due to defragmentation or other relocations. Thus handles
    // must be held open until location information for both paths has
    // been retrieved.
    handle_wrapper p1(
      create_file(
          ph1.c_str(),
          0,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0 ) );
    int error1(0); // save error code in case we have to throw
    if ( p1.handle == INVALID_HANDLE_VALUE )
      error1 = ::GetLastError();
    handle_wrapper p2(
      create_file(
          ph2.c_str(),
          0,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0 ) );
    if ( p1.handle == INVALID_HANDLE_VALUE
      || p2.handle == INVALID_HANDLE_VALUE )
    {
      if ( p1.handle != INVALID_HANDLE_VALUE
        || p2.handle != INVALID_HANDLE_VALUE )
        { return std::make_pair( ok, false ); }
      BOOST_ASSERT( p1.handle == INVALID_HANDLE_VALUE
        && p2.handle == INVALID_HANDLE_VALUE );
        { return std::make_pair( error_code( error1, system_category()), false ); }
    }
    // at this point, both handles are known to be valid
    BY_HANDLE_FILE_INFORMATION info1, info2;
    if ( !::GetFileInformationByHandle( p1.handle, &info1 ) )
      { return std::make_pair( error_code( ::GetLastError(), system_category() ), false ); }
    if ( !::GetFileInformationByHandle( p2.handle, &info2 ) )
      { return std::make_pair( error_code( ::GetLastError(), system_category() ), false ); }
    // In theory, volume serial numbers are sufficient to distinguish between
    // devices, but in practice VSN's are sometimes duplicated, so last write
    // time and file size are also checked.
      return std::make_pair( ok,
        info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber
        && info1.nFileIndexHigh == info2.nFileIndexHigh
        && info1.nFileIndexLow == info2.nFileIndexLow
        && info1.nFileSizeHigh == info2.nFileSizeHigh
        && info1.nFileSizeLow == info2.nFileSizeLow
        && info1.ftLastWriteTime.dwLowDateTime
          == info2.ftLastWriteTime.dwLowDateTime
        && info1.ftLastWriteTime.dwHighDateTime
          == info2.ftLastWriteTime.dwHighDateTime );
  }

  template< class String >
  boost::filesystem2::detail::uintmax_pair
  file_size_template( const String & ph )
  {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    // by now, intmax_t is 64-bits on all Windows compilers
    if ( get_file_attributes_ex( ph.c_str(), fad ) == 0 )
      return std::make_pair( error_code( ::GetLastError(), system_category() ), 0 );    
    if ( (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) !=0 )
      return std::make_pair( error_code( ERROR_FILE_NOT_FOUND, system_category()), 0 );
    return std::make_pair( ok,
      (static_cast<boost::uintmax_t>(fad.nFileSizeHigh)
        << (sizeof(fad.nFileSizeLow)*8))
      + fad.nFileSizeLow );
  }

  inline bool get_free_disk_space( const std::string & ph,
    PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free )
    { return ::GetDiskFreeSpaceExA( ph.c_str(), avail, total, free ) != 0; }

  template< class String >
  boost::filesystem2::detail::space_pair
  space_template( String & ph )
  {
    ULARGE_INTEGER avail, total, free;
    boost::filesystem2::detail::space_pair result;
    if ( get_free_disk_space( ph, &avail, &total, &free ) )
    {
      result.first = ok;
      result.second.capacity
        = (static_cast<boost::uintmax_t>(total.HighPart) << 32)
          + total.LowPart;
      result.second.free
        = (static_cast<boost::uintmax_t>(free.HighPart) << 32)
          + free.LowPart;
      result.second.available
        = (static_cast<boost::uintmax_t>(avail.HighPart) << 32)
          + avail.LowPart;
    }
    else
    {
      result.first = error_code( ::GetLastError(), system_category() );
      result.second.capacity = result.second.free
        = result.second.available = 0;
    }
    return result;
  }

  inline DWORD get_current_directory( DWORD sz, char * buf )
    { return ::GetCurrentDirectoryA( sz, buf ); } 

  template< class String >
  error_code
  get_current_path_template( String & ph )
  {
    DWORD sz;
    if ( (sz = get_current_directory( 0,
      static_cast<typename String::value_type*>(0) )) == 0 )
      { sz = 1; }
    typedef typename String::value_type value_type;
    boost::scoped_array<value_type> buf( new value_type[sz] );
    if ( get_current_directory( sz, buf.get() ) == 0 )
      return error_code( ::GetLastError(), system_category() );
    ph = buf.get();
    return ok;
  }

  inline bool set_current_directory( const char * buf )
    { return ::SetCurrentDirectoryA( buf ) != 0; } 

  template< class String >
  error_code
  set_current_path_template( const String & ph )
  {
    return error_code( set_current_directory( ph.c_str() )
      ? 0 : ::GetLastError(), system_category() );
  }

  inline std::size_t get_full_path_name(
    const std::string & ph, std::size_t len, char * buf, char ** p )
  {
    return static_cast<std::size_t>(
      ::GetFullPathNameA( ph.c_str(),
        static_cast<DWORD>(len), buf, p ));
  } 

  const std::size_t buf_size( 128 );

  template<class String>
  error_code
  get_full_path_name_template( const String & ph, String & target )
  {
    typename String::value_type buf[buf_size];
    typename String::value_type * pfn;
    std::size_t len = get_full_path_name( ph,
      buf_size , buf, &pfn );
    if ( len == 0 ) return error_code( ::GetLastError(), system_category() );
    if ( len > buf_size )
    {
      typedef typename String::value_type value_type;
      boost::scoped_array<value_type> big_buf( new value_type[len] );
      if ( (len=get_full_path_name( ph, len , big_buf.get(), &pfn ))
        == 0 ) return error_code( ::GetLastError(), system_category() );
      big_buf[len] = '\0';
      target = big_buf.get();
      return ok;
    }
    buf[len] = '\0';
    target = buf;
    return ok;
  }

  template<class String>
  error_code
  get_file_write_time( const String & ph, FILETIME & last_write_time )
  {
    handle_wrapper hw(
      create_file( ph.c_str(), 0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0 ) );
    if ( hw.handle == INVALID_HANDLE_VALUE )
      return error_code( ::GetLastError(), system_category() );
    return error_code( ::GetFileTime( hw.handle, 0, 0, &last_write_time ) != 0
      ? 0 : ::GetLastError(), system_category() );
  }

  template<class String>
  error_code
  set_file_write_time( const String & ph, const FILETIME & last_write_time )
  {
    handle_wrapper hw(
      create_file( ph.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0 ) );
    if ( hw.handle == INVALID_HANDLE_VALUE )
      return error_code( ::GetLastError(), system_category() );
    return error_code( ::SetFileTime( hw.handle, 0, 0, &last_write_time ) != 0
      ? 0 : ::GetLastError(), system_category() );
  }

  // these constants come from inspecting some Microsoft sample code
  std::time_t to_time_t( const FILETIME & ft )
  {
    __int64 t = (static_cast<__int64>( ft.dwHighDateTime ) << 32)
      + ft.dwLowDateTime;
# if !defined( BOOST_MSVC ) || BOOST_MSVC > 1300 // > VC++ 7.0
    t -= 116444736000000000LL;
# else
    t -= 116444736000000000;
# endif
    t /= 10000000;
    return static_cast<std::time_t>( t );
  }

  void to_FILETIME( std::time_t t, FILETIME & ft )
  {
    __int64 temp = t;
    temp *= 10000000;
# if !defined( BOOST_MSVC ) || BOOST_MSVC > 1300 // > VC++ 7.0
    temp += 116444736000000000LL;
# else
    temp += 116444736000000000;
# endif
    ft.dwLowDateTime = static_cast<DWORD>( temp );
    ft.dwHighDateTime = static_cast<DWORD>( temp >> 32 );
  }

  template<class String>
  boost::filesystem2::detail::time_pair
  last_write_time_template( const String & ph )
  {
    FILETIME lwt;
    error_code ec(
      get_file_write_time( ph, lwt ) );
    return std::make_pair( ec, to_time_t( lwt ) );
  }

  template<class String>
  error_code
  last_write_time_template( const String & ph, const std::time_t new_time )
  {
    FILETIME lwt;
    to_FILETIME( new_time, lwt );
    return set_file_write_time( ph, lwt );
  }

  bool remove_directory( const std::string & ph )
    { return ::RemoveDirectoryA( ph.c_str() ) != 0; }
  
  bool delete_file( const std::string & ph )
    { return ::DeleteFileA( ph.c_str() ) != 0; }
  
  template<class String>
  error_code
  remove_template( const String & ph )
  {
    // TODO: test this code in the presence of Vista symlinks,
    // including dangling, self-referal, and cyclic symlinks
    error_code ec;
    fs::file_status sf( fs::detail::status_api( ph, ec ) );
    if ( ec ) 
      return ec;
    if ( sf.type() == fs::file_not_found )
      return ok;
    if ( fs::is_directory( sf ) )
    {
      if ( !remove_directory( ph ) )
        return error_code(::GetLastError(), system_category());
    }
    else
    {
      if ( !delete_file( ph ) ) return error_code(::GetLastError(), system_category());
    }
    return ok;
  }

  inline bool create_directory( const std::string & dir )
    {  return ::CreateDirectoryA( dir.c_str(), 0 ) != 0; }
         
  template<class String>
  boost::filesystem2::detail::query_pair
  create_directory_template( const String & dir_ph )
  {
    error_code error, dummy;
    if ( create_directory( dir_ph ) ) return std::make_pair( error, true );
    error = error_code( ::GetLastError(), system_category() );
    // an error here may simply mean the postcondition is already met
    if ( error.value() == ERROR_ALREADY_EXISTS
      && fs::is_directory( fs::detail::status_api( dir_ph, dummy ) ) )
      return std::make_pair( ok, false );
    return std::make_pair( error, false );
  }

#if _WIN32_WINNT >= 0x500
  inline bool create_hard_link( const std::string & to_ph,
    const std::string & from_ph )
    {  return ::CreateHardLinkA( from_ph.c_str(), to_ph.c_str(), 0 ) != 0; }
#endif
  
#if _WIN32_WINNT >= 0x500
  template<class String>
  error_code
  create_hard_link_template( const String & to_ph,
    const String & from_ph )
  {
    return error_code( create_hard_link( to_ph.c_str(), from_ph.c_str() )
      ? 0 : ::GetLastError(), system_category() );
  }
#endif

#else // BOOST_POSIX_API

  int posix_remove( const char * p )
  {
#     if defined(__QNXNTO__) || (defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)))
        // Some Metrowerks C library versions fail on directories because of a
        // known Metrowerks coding error in ::remove. Workaround is to call
        // rmdir() or unlink() as indicated.
        // Same bug also reported for QNX, with the same fix.
        int err = ::unlink( p );
        if ( err == 0 || errno != EPERM )
          return err;
        return ::rmdir( p );
#     else
        return std::remove( p );
#     endif
  }

#endif
} // unnamed namespace

namespace boost
{
  namespace filesystem2
  {
    namespace detail
    {
      BOOST_FILESYSTEM_DECL system::error_code throws;

//  free functions  ----------------------------------------------------------//

      BOOST_FILESYSTEM_DECL error_code not_found_error()
      {
#     ifdef BOOST_WINDOWS_API
        return error_code(ERROR_PATH_NOT_FOUND, system_category());
#     else
        return error_code(ENOENT, system_category()); 
#     endif
      }

      BOOST_FILESYSTEM_DECL bool possible_large_file_size_support()
      {
#   ifdef BOOST_POSIX_API
        struct stat lcl_stat;
        return sizeof( lcl_stat.st_size ) > 4;
#   else
        return true;
#   endif
      }

#   ifdef BOOST_WINDOWS_API

      BOOST_FILESYSTEM_DECL fs::file_status
        status_api( const std::string & ph, error_code & ec )
        { return status_template( ph, ec ); }

#     ifndef BOOST_FILESYSTEM2_NARROW_ONLY

      BOOST_FILESYSTEM_DECL fs::file_status
      status_api( const std::wstring & ph, error_code & ec )
        { return status_template( ph, ec ); }

      BOOST_FILESYSTEM_DECL bool symbolic_link_exists_api( const std::wstring & )
        { return false; }

      BOOST_FILESYSTEM_DECL
      fs::detail::query_pair is_empty_api( const std::wstring & ph )
        { return is_empty_template( ph ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::query_pair
      equivalent_api( const std::wstring & ph1, const std::wstring & ph2 )
        { return equivalent_template( ph1, ph2 ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::uintmax_pair file_size_api( const std::wstring & ph )
        { return file_size_template( ph ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::space_pair space_api( const std::wstring & ph )
        { return space_template( ph ); }

      BOOST_FILESYSTEM_DECL
      error_code 
      get_current_path_api( std::wstring & ph )
        { return get_current_path_template( ph ); }

      BOOST_FILESYSTEM_DECL
      error_code 
      set_current_path_api( const std::wstring & ph )
        { return set_current_path_template( ph ); }

      BOOST_FILESYSTEM_DECL error_code
        get_full_path_name_api( const std::wstring & ph, std::wstring & target )
         { return get_full_path_name_template( ph, target ); }

      BOOST_FILESYSTEM_DECL time_pair
        last_write_time_api( const std::wstring & ph )
          { return last_write_time_template( ph ); }
 
      BOOST_FILESYSTEM_DECL error_code
        last_write_time_api( const std::wstring & ph, std::time_t new_value )
          { return last_write_time_template( ph, new_value ); }

      BOOST_FILESYSTEM_DECL fs::detail::query_pair
      create_directory_api( const std::wstring & ph )
        { return create_directory_template( ph ); }

#if _WIN32_WINNT >= 0x500
      BOOST_FILESYSTEM_DECL error_code
      create_hard_link_api( const std::wstring & to_ph,
        const std::wstring & from_ph )
        { return create_hard_link_template( to_ph, from_ph ); }
#endif
      
      BOOST_FILESYSTEM_DECL error_code
      create_symlink_api( const std::wstring & /*to_ph*/,
        const std::wstring & /*from_ph*/ )
        { return error_code( ERROR_NOT_SUPPORTED, system_category() ); }

      BOOST_FILESYSTEM_DECL error_code
      remove_api( const std::wstring & ph ) { return remove_template( ph ); }

      BOOST_FILESYSTEM_DECL error_code
      rename_api( const std::wstring & from, const std::wstring & to )
      {
        return error_code( ::MoveFileW( from.c_str(), to.c_str() )
          ? 0 : ::GetLastError(), system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code
      copy_file_api( const std::wstring & from, const std::wstring & to, bool fail_if_exists )
      {
        return error_code( ::CopyFileW( from.c_str(), to.c_str(), fail_if_exists )
          ? 0 : ::GetLastError(), system_category() );
      }

      BOOST_FILESYSTEM_DECL bool create_file_api( const std::wstring & ph,
        std::ios_base::openmode mode ) // true if succeeds
      {
        DWORD access(
          ((mode & std::ios_base::in) == 0 ? 0 : GENERIC_READ)
          | ((mode & std::ios_base::out) == 0 ? 0 : GENERIC_WRITE) );

        DWORD disposition(0); // see 27.8.1.3 Table 92
        if ( (mode&~std::ios_base::binary)
          == (std::ios_base::out|std::ios_base::app) )
          disposition = OPEN_ALWAYS;
        else if ( (mode&~(std::ios_base::binary|std::ios_base::out))
          == std::ios_base::in ) disposition = OPEN_EXISTING;
        else if ( ((mode&~(std::ios_base::binary|std::ios_base::trunc))
          == std::ios_base::out )
          || ((mode&~std::ios_base::binary)
          == (std::ios_base::in|std::ios_base::out|std::ios_base::trunc)) )
          disposition = CREATE_ALWAYS;
        else BOOST_ASSERT( 0 && "invalid mode argument" );

        HANDLE handle ( ::CreateFileW( ph.c_str(), access,
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
          disposition, (mode &std::ios_base::out) != 0
          ? FILE_ATTRIBUTE_ARCHIVE : FILE_ATTRIBUTE_NORMAL, 0 ) );
        if ( handle == INVALID_HANDLE_VALUE ) return false;
        ::CloseHandle( handle );
        return true;
      }

      BOOST_FILESYSTEM_DECL std::string narrow_path_api(
        const std::wstring & ph ) // return is empty if fails
      {
        std::string narrow_short_form;
        std::wstring short_form;
        for ( DWORD buf_sz( static_cast<DWORD>( ph.size()+1 ));; )
        {
          boost::scoped_array<wchar_t> buf( new wchar_t[buf_sz] );
          DWORD sz( ::GetShortPathNameW( ph.c_str(), buf.get(), buf_sz ) );
          if ( sz == 0 ) return narrow_short_form;
          if ( sz <= buf_sz )
          {
            short_form += buf.get();
            break;
          }
          buf_sz = sz + 1;
        }
        // contributed by Takeshi Mouri:
        int narrow_sz( ::WideCharToMultiByte( CP_ACP, 0,
          short_form.c_str(), static_cast<int>(short_form.size()), 0, 0, 0, 0 ) );
        boost::scoped_array<char> narrow_buf( new char[narrow_sz] );
        ::WideCharToMultiByte( CP_ACP, 0,
          short_form.c_str(), static_cast<int>(short_form.size()),
          narrow_buf.get(), narrow_sz, 0, 0 );
        narrow_short_form.assign(narrow_buf.get(), narrow_sz);

        return narrow_short_form;
      }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_first( void *& handle, const std::wstring & dir,
        std::wstring & target, file_status & sf, file_status & symlink_sf )
      {
        // use a form of search Sebastian Martel reports will work with Win98
        std::wstring dirpath( dir );
        dirpath += (dirpath.empty()
          || dirpath[dirpath.size()-1] != L'\\') ? L"\\*" : L"*";

        WIN32_FIND_DATAW data;
        if ( (handle = ::FindFirstFileW( dirpath.c_str(), &data ))
          == INVALID_HANDLE_VALUE )
        { 
          handle = 0;
          return error_code( ::GetLastError() == ERROR_FILE_NOT_FOUND
            ? 0 : ::GetLastError(), system_category() );
        }
        target = data.cFileName;
        if ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
          { sf.type( directory_file ); symlink_sf.type( directory_file ); }
        else { sf.type( regular_file ); symlink_sf.type( regular_file ); }
        return ok;
      }  

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_increment( void *& handle, std::wstring & target,
        file_status & sf, file_status & symlink_sf )
      {
        WIN32_FIND_DATAW data;
        if ( ::FindNextFileW( handle, &data ) == 0 ) // fails
        {
          int error = ::GetLastError();
          dir_itr_close( handle );
          return error_code( error == ERROR_NO_MORE_FILES ? 0 : error, system_category() );
        }
        target = data.cFileName;
        if ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
          { sf.type( directory_file ); symlink_sf.type( directory_file ); }
        else { sf.type( regular_file ); symlink_sf.type( regular_file ); }
        return ok;
      }

#     endif // ifndef BOOST_FILESYSTEM2_NARROW_ONLY

      // suggested by Walter Landry
      BOOST_FILESYSTEM_DECL bool symbolic_link_exists_api( const std::string & )
        { return false; }

      BOOST_FILESYSTEM_DECL
      fs::detail::query_pair is_empty_api( const std::string & ph )
        { return is_empty_template( ph ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::query_pair
      equivalent_api( const std::string & ph1, const std::string & ph2 )
        { return equivalent_template( ph1, ph2 ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::uintmax_pair file_size_api( const std::string & ph )
        { return file_size_template( ph ); }

      BOOST_FILESYSTEM_DECL
      fs::detail::space_pair space_api( const std::string & ph )
        { return space_template( ph ); }

      BOOST_FILESYSTEM_DECL
      error_code 
      get_current_path_api( std::string & ph )
        { return get_current_path_template( ph ); }

      BOOST_FILESYSTEM_DECL
      error_code 
      set_current_path_api( const std::string & ph )
        { return set_current_path_template( ph ); }

      BOOST_FILESYSTEM_DECL error_code
        get_full_path_name_api( const std::string & ph, std::string & target )
         { return get_full_path_name_template( ph, target ); }

      BOOST_FILESYSTEM_DECL time_pair
        last_write_time_api( const std::string & ph )
          { return last_write_time_template( ph ); }
 
      BOOST_FILESYSTEM_DECL error_code
        last_write_time_api( const std::string & ph, std::time_t new_value )
          { return last_write_time_template( ph, new_value ); }

      BOOST_FILESYSTEM_DECL fs::detail::query_pair
      create_directory_api( const std::string & ph )
        { return create_directory_template( ph ); }

#if _WIN32_WINNT >= 0x500
      BOOST_FILESYSTEM_DECL error_code
      create_hard_link_api( const std::string & to_ph,
        const std::string & from_ph )
      { 
        return create_hard_link_template( to_ph, from_ph );
      }
#endif

      BOOST_FILESYSTEM_DECL error_code
      create_symlink_api( const std::string & /*to_ph*/,
        const std::string & /*from_ph*/ )
        { return error_code( ERROR_NOT_SUPPORTED, system_category() ); }

      BOOST_FILESYSTEM_DECL error_code
      remove_api( const std::string & ph ) { return remove_template( ph ); }

      BOOST_FILESYSTEM_DECL error_code
      rename_api( const std::string & from, const std::string & to )
      {
        return error_code( ::MoveFileA( from.c_str(), to.c_str() )
          ? 0 : ::GetLastError(), system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code
      copy_file_api( const std::string & from, const std::string & to, bool fail_if_exists )
      {
        return error_code( ::CopyFileA( from.c_str(), to.c_str(), fail_if_exists )
          ? 0 : ::GetLastError(), system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_first( void *& handle, const std::string & dir,
        std::string & target, file_status & sf, file_status & symlink_sf )
      // Note: an empty root directory has no "." or ".." entries, so this
      // causes a ERROR_FILE_NOT_FOUND error which we do not considered an
      // error. It is treated as eof instead.
      {
        // use a form of search Sebastian Martel reports will work with Win98
        std::string dirpath( dir );
        dirpath += (dirpath.empty()
          || (dirpath[dirpath.size()-1] != '\\'
            && dirpath[dirpath.size()-1] != ':')) ? "\\*" : "*";

        WIN32_FIND_DATAA data;
        if ( (handle = ::FindFirstFileA( dirpath.c_str(), &data ))
          == INVALID_HANDLE_VALUE )
        { 
          handle = 0;
          return error_code( (::GetLastError() == ERROR_FILE_NOT_FOUND
                           // Windows Mobile returns ERROR_NO_MORE_FILES; see ticket #3551                                           
                           || ::GetLastError() == ERROR_NO_MORE_FILES) 
            ? 0 : ::GetLastError(), system_category() );
        }
        target = data.cFileName;
        if ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
          { sf.type( directory_file ); symlink_sf.type( directory_file ); }
        else { sf.type( regular_file ); symlink_sf.type( regular_file ); }
        return ok;
      }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_close( void *& handle )
      {
        if ( handle != 0 )
        {
          bool ok = ::FindClose( handle ) != 0;
          handle = 0;
          return error_code( ok ? 0 : ::GetLastError(), system_category() );
        }
        return ok;
      }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_increment( void *& handle, std::string & target,
        file_status & sf, file_status & symlink_sf )
      {
        WIN32_FIND_DATAA data;
        if ( ::FindNextFileA( handle, &data ) == 0 ) // fails
        {
          int error = ::GetLastError();
          dir_itr_close( handle );
          return error_code( error == ERROR_NO_MORE_FILES ? 0 : error, system_category() );
        }
        target = data.cFileName;
        if ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
          { sf.type( directory_file ); symlink_sf.type( directory_file ); }
        else { sf.type( regular_file ); symlink_sf.type( regular_file ); }
        return ok;
      }

#   else // BOOST_POSIX_API

      BOOST_FILESYSTEM_DECL fs::file_status
      status_api( const std::string & ph, error_code & ec )
      {
        struct stat path_stat;
        if ( ::stat( ph.c_str(), &path_stat ) != 0 )
        {
          if ( errno == ENOENT || errno == ENOTDIR )
          {
            ec = ok;
            return fs::file_status( fs::file_not_found );
          }
          ec = error_code( errno, system_category() );
          return fs::file_status( fs::status_unknown );
        }
        ec = ok;
        if ( S_ISDIR( path_stat.st_mode ) )
          return fs::file_status( fs::directory_file );
        if ( S_ISREG( path_stat.st_mode ) )
          return fs::file_status( fs::regular_file );
        if ( S_ISBLK( path_stat.st_mode ) )
          return fs::file_status( fs::block_file );
        if ( S_ISCHR( path_stat.st_mode ) )
          return fs::file_status( fs::character_file );
        if ( S_ISFIFO( path_stat.st_mode ) )
          return fs::file_status( fs::fifo_file );
        if ( S_ISSOCK( path_stat.st_mode ) )
          return fs::file_status( fs::socket_file );
        return fs::file_status( fs::type_unknown );
      }

      BOOST_FILESYSTEM_DECL fs::file_status
      symlink_status_api( const std::string & ph, error_code & ec )
      {
        struct stat path_stat;
        if ( ::lstat( ph.c_str(), &path_stat ) != 0 )
        {
          if ( errno == ENOENT || errno == ENOTDIR )
          {
            ec = ok;
            return fs::file_status( fs::file_not_found );
          }
          ec = error_code( errno, system_category() );
          return fs::file_status( fs::status_unknown );
        }
        ec = ok;
        if ( S_ISREG( path_stat.st_mode ) )
          return fs::file_status( fs::regular_file );
        if ( S_ISDIR( path_stat.st_mode ) )
          return fs::file_status( fs::directory_file );
        if ( S_ISLNK( path_stat.st_mode ) )
          return fs::file_status( fs::symlink_file );
        if ( S_ISBLK( path_stat.st_mode ) )
          return fs::file_status( fs::block_file );
        if ( S_ISCHR( path_stat.st_mode ) )
          return fs::file_status( fs::character_file );
        if ( S_ISFIFO( path_stat.st_mode ) )
          return fs::file_status( fs::fifo_file );
        if ( S_ISSOCK( path_stat.st_mode ) )
          return fs::file_status( fs::socket_file );
        return fs::file_status( fs::type_unknown );
      }

      // suggested by Walter Landry
      BOOST_FILESYSTEM_DECL bool
      symbolic_link_exists_api( const std::string & ph )
      {
        struct stat path_stat;
        return ::lstat( ph.c_str(), &path_stat ) == 0
          && S_ISLNK( path_stat.st_mode );
      }

      BOOST_FILESYSTEM_DECL query_pair
      is_empty_api( const std::string & ph )
      {
        struct stat path_stat;
        if ( (::stat( ph.c_str(), &path_stat )) != 0 )
          return std::make_pair( error_code( errno, system_category() ), false );        
        return std::make_pair( ok, S_ISDIR( path_stat.st_mode )
          ? is_empty_directory( ph )
          : path_stat.st_size == 0 );
      }

      BOOST_FILESYSTEM_DECL query_pair
      equivalent_api( const std::string & ph1, const std::string & ph2 )
      {
        struct stat s2;
        int e2( ::stat( ph2.c_str(), &s2 ) );
        struct stat s1;
        int e1( ::stat( ph1.c_str(), &s1 ) );
        if ( e1 != 0 || e2 != 0 )
          return std::make_pair( error_code( e1 != 0 && e2 != 0 ? errno : 0, system_category() ), false );
        // at this point, both stats are known to be valid
        return std::make_pair( ok,
            s1.st_dev == s2.st_dev
            && s1.st_ino == s2.st_ino
            // According to the POSIX stat specs, "The st_ino and st_dev fields
            // taken together uniquely identify the file within the system."
            // Just to be sure, size and mod time are also checked.
            && s1.st_size == s2.st_size
            && s1.st_mtime == s2.st_mtime );
      }
 
      BOOST_FILESYSTEM_DECL uintmax_pair
      file_size_api( const std::string & ph )
      {
        struct stat path_stat;
        if ( ::stat( ph.c_str(), &path_stat ) != 0 )
          return std::make_pair( error_code( errno, system_category() ), 0 );
        if ( !S_ISREG( path_stat.st_mode ) )
          return std::make_pair( error_code( EPERM, system_category() ), 0 ); 
        return std::make_pair( ok,
          static_cast<boost::uintmax_t>(path_stat.st_size) );
      }

      BOOST_FILESYSTEM_DECL space_pair
      space_api( const std::string & ph )
      {
        struct BOOST_STATVFS vfs;
        space_pair result;
        if ( ::BOOST_STATVFS( ph.c_str(), &vfs ) != 0 )
        {
          result.first = error_code( errno, system_category() );
          result.second.capacity = result.second.free
            = result.second.available = 0;
        }
        else
        {
          result.first = ok;
          result.second.capacity 
            = static_cast<boost::uintmax_t>(vfs.f_blocks) * BOOST_STATVFS_F_FRSIZE;
          result.second.free 
            = static_cast<boost::uintmax_t>(vfs.f_bfree) * BOOST_STATVFS_F_FRSIZE;
          result.second.available
            = static_cast<boost::uintmax_t>(vfs.f_bavail) * BOOST_STATVFS_F_FRSIZE;
        }
        return result;
      }

      BOOST_FILESYSTEM_DECL time_pair 
      last_write_time_api( const std::string & ph )
      {
        struct stat path_stat;
        if ( ::stat( ph.c_str(), &path_stat ) != 0 )
          return std::make_pair( error_code( errno, system_category() ), 0 );
        return std::make_pair( ok, path_stat.st_mtime );
      }

      BOOST_FILESYSTEM_DECL error_code
      last_write_time_api( const std::string & ph, std::time_t new_value )
      {
        struct stat path_stat;
        if ( ::stat( ph.c_str(), &path_stat ) != 0 )
          return error_code( errno, system_category() );
        ::utimbuf buf;
        buf.actime = path_stat.st_atime; // utime() updates access time too:-(
        buf.modtime = new_value;
        return error_code( ::utime( ph.c_str(), &buf ) != 0 ? errno : 0, system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code 
      get_current_path_api( std::string & ph )
      {
        for ( long path_max = 32;; path_max *=2 ) // loop 'til buffer large enough
        {
          boost::scoped_array<char>
            buf( new char[static_cast<std::size_t>(path_max)] );
          if ( ::getcwd( buf.get(), static_cast<std::size_t>(path_max) ) == 0 )
          {
            if ( errno != ERANGE
          // bug in some versions of the Metrowerks C lib on the Mac: wrong errno set 
#         if defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
              && errno != 0
#         endif
              ) return error_code( errno, system_category() );
          }
          else
          {
            ph = buf.get();
            break;
          }
        }
        return ok;
      }

      BOOST_FILESYSTEM_DECL error_code
      set_current_path_api( const std::string & ph )
      {
        return error_code( ::chdir( ph.c_str() )
          ? errno : 0, system_category() );
      }

      BOOST_FILESYSTEM_DECL fs::detail::query_pair
      create_directory_api( const std::string & ph )
      {
        if ( ::mkdir( ph.c_str(), S_IRWXU|S_IRWXG|S_IRWXO ) == 0 )
          { return std::make_pair( ok, true ); }
        int ec=errno;
        error_code dummy;
        if ( ec != EEXIST 
          || !fs::is_directory( status_api( ph, dummy ) ) )
          { return std::make_pair( error_code( ec, system_category() ), false ); }
        return std::make_pair( ok, false );
      }

      BOOST_FILESYSTEM_DECL error_code
      create_hard_link_api( const std::string & to_ph,
          const std::string & from_ph )
      {
        return error_code( ::link( to_ph.c_str(), from_ph.c_str() ) == 0
          ? 0 : errno, system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code
      create_symlink_api( const std::string & to_ph,
          const std::string & from_ph )
      {
        return error_code( ::symlink( to_ph.c_str(), from_ph.c_str() ) == 0
          ? 0 : errno, system_category() ); 
      }

      BOOST_FILESYSTEM_DECL error_code
      remove_api( const std::string & ph )
      {
        if ( posix_remove( ph.c_str() ) == 0 )
          return ok;
        int error = errno;
        // POSIX says "If the directory is not an empty directory, rmdir()
        // shall fail and set errno to EEXIST or ENOTEMPTY."
        // Linux uses ENOTEMPTY, Solaris uses EEXIST.
        if ( error == EEXIST ) error = ENOTEMPTY;

        error_code ec;

        // ignore errors if post-condition satisfied
        return status_api(ph, ec).type() == file_not_found
          ? ok : error_code( error, system_category() ) ;
      }

      BOOST_FILESYSTEM_DECL error_code
      rename_api( const std::string & from, const std::string & to )
      {
        // POSIX is too permissive so must check
        error_code dummy;
        if ( fs::exists( status_api( to, dummy ) ) ) 
          return error_code( EEXIST, system_category() );
        return error_code( std::rename( from.c_str(), to.c_str() ) != 0 
          ? errno : 0, system_category() );
      }

      BOOST_FILESYSTEM_DECL error_code
      copy_file_api( const std::string & from_file_ph,
        const std::string & to_file_ph, bool fail_if_exists )
      {
        const std::size_t buf_sz = 32768;
        boost::scoped_array<char> buf( new char [buf_sz] );
        int infile=-1, outfile=-1;  // -1 means not open

        // bug fixed: code previously did a stat() on the from_file first, but that
        // introduced a gratuitous race condition; the stat() is now done after the open()

        if ( (infile = ::open( from_file_ph.c_str(), O_RDONLY )) < 0 )
          { return error_code( errno, system_category() ); }

        struct stat from_stat;
        if ( ::stat( from_file_ph.c_str(), &from_stat ) != 0 )
        { 
          ::close(infile);
          return error_code( errno, system_category() );
        }

        int oflag = O_CREAT | O_WRONLY | O_TRUNC;
        if ( fail_if_exists )
          oflag |= O_EXCL;
        if (  (outfile = ::open( to_file_ph.c_str(), oflag, from_stat.st_mode )) < 0 )
        {
          int open_errno = errno;
          BOOST_ASSERT( infile >= 0 );
          ::close( infile );
          return error_code( open_errno, system_category() );
        }

        ssize_t sz, sz_read=1, sz_write;
        while ( sz_read > 0
          && (sz_read = ::read( infile, buf.get(), buf_sz )) > 0 )
        {
          // Allow for partial writes - see Advanced Unix Programming (2nd Ed.),
          // Marc Rochkind, Addison-Wesley, 2004, page 94
          sz_write = 0;
          do
          {
            if ( (sz = ::write( outfile, buf.get() + sz_write,
              sz_read - sz_write )) < 0 )
            { 
              sz_read = sz; // cause read loop termination
              break;        //  and error to be thrown after closes
            }
            sz_write += sz;
          } while ( sz_write < sz_read );
        }

        if ( ::close( infile) < 0 ) sz_read = -1;
        if ( ::close( outfile) < 0 ) sz_read = -1;

        return error_code( sz_read < 0 ? errno : 0, system_category() );
      }

      // this code is based on Stevens and Rago, Advanced Programming in the
      // UNIX envirnment, 2nd Ed., ISBN 0-201-43307-9, page 49
      error_code path_max( std::size_t & result )
      {
#     ifdef PATH_MAX
        static std::size_t max = PATH_MAX;
#     else
        static std::size_t max = 0;
#     endif
        if ( max == 0 )
        {
          errno = 0;
          long tmp = ::pathconf( "/", _PC_NAME_MAX );
          if ( tmp < 0 )
          {
            if ( errno == 0 ) // indeterminate
              max = 4096; // guess
            else return error_code( errno, system_category() );
          }
          else max = static_cast<std::size_t>( tmp + 1 ); // relative root
        }
        result = max;
        return ok;
      }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_first( void *& handle, void *& buffer,
        const std::string & dir, std::string & target,
        file_status &, file_status & )
      {
        if ( (handle = ::opendir( dir.c_str() )) == 0 )
          return error_code( errno, system_category() );
        target = std::string( "." ); // string was static but caused trouble
                                     // when iteration called from dtor, after
                                     // static had already been destroyed
        std::size_t path_size (0);  // initialization quiets gcc warning
        error_code ec = path_max( path_size );
        if ( ec ) return ec;
        dirent de;
        buffer = std::malloc( (sizeof(dirent) - sizeof(de.d_name))
          +  path_size + 1 ); // + 1 for "/0"
        return ok;
      }  

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_close( void *& handle, void*& buffer )
      {
        std::free( buffer );
        buffer = 0;
        if ( handle == 0 ) return ok;
        DIR * h( static_cast<DIR*>(handle) );
        handle = 0;
        return error_code( ::closedir( h ) == 0 ? 0 : errno, system_category() );
      }

#if defined(__PGI) && defined(__USE_FILE_OFFSET64)
#define dirent dirent64
#endif

      // warning: the only dirent member updated is d_name
      inline int readdir_r_simulator( DIR * dirp, struct dirent * entry,
        struct dirent ** result ) // *result set to 0 on end of directory
        {
          errno = 0;

    #     if !defined(__CYGWIN__) \
          && defined(_POSIX_THREAD_SAFE_FUNCTIONS) \
          && defined(_SC_THREAD_SAFE_FUNCTIONS) \
          && (_POSIX_THREAD_SAFE_FUNCTIONS+0 >= 0) \
          && (!defined(__hpux) || defined(_REENTRANT)) \
          && (!defined(_AIX) || defined(__THREAD_SAFE))
          if ( ::sysconf( _SC_THREAD_SAFE_FUNCTIONS ) >= 0 )
            { return ::readdir_r( dirp, entry, result ); }
    #     endif

          struct dirent * p;
          *result = 0;
          if ( (p = ::readdir( dirp )) == 0 )
            return errno;
          std::strcpy( entry->d_name, p->d_name );
          *result = entry;
          return 0;
        }

      BOOST_FILESYSTEM_DECL error_code
      dir_itr_increment( void *& handle, void *& buffer,
        std::string & target, file_status & sf, file_status & symlink_sf )
      {
        BOOST_ASSERT( buffer != 0 );
        dirent * entry( static_cast<dirent *>(buffer) );
        dirent * result;
        int return_code;
        if ( (return_code = readdir_r_simulator( static_cast<DIR*>(handle),
          entry, &result )) != 0 ) return error_code( errno, system_category() );
        if ( result == 0 ) return dir_itr_close( handle, buffer );
        target = entry->d_name;
#     ifdef BOOST_FILESYSTEM_STATUS_CACHE
        if ( entry->d_type == DT_UNKNOWN )  // filesystem does not supply d_type value
        {
          sf = symlink_sf = fs::file_status(fs::status_unknown);
        }
        else  // filesystem supplies d_type value
        {
          if ( entry->d_type == DT_DIR )
            sf = symlink_sf = fs::file_status( fs::directory_file );
          else if ( entry->d_type == DT_REG )
            sf = symlink_sf = fs::file_status( fs::regular_file );
          else if ( entry->d_type == DT_LNK )
          {
            sf = fs::file_status( fs::status_unknown );
            symlink_sf = fs::file_status( fs::symlink_file );
          }
          else sf = symlink_sf = fs::file_status( fs::status_unknown );
        }
#     else
        sf = symlink_sf = fs::file_status( fs::status_unknown );
#     endif
        return ok;
      }

#   endif
    } // namespace detail
  } // namespace filesystem2
} // namespace boost
