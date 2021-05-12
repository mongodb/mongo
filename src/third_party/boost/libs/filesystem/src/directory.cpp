//  directory.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Copyright 2019 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>

#include <cstddef>
#include <cerrno>
#include <cstring>
#include <cstdlib> // std::malloc, std::free
#include <new> // std::nothrow, std::bad_alloc
#include <limits>
#include <string>
#include <utility> // std::move
#include <boost/assert.hpp>
#include <boost/system/error_code.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#ifdef BOOST_POSIX_API

#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && (_POSIX_THREAD_SAFE_FUNCTIONS+0 >= 0)\
  && defined(_SC_THREAD_SAFE_FUNCTIONS)\
  && !defined(__CYGWIN__)\
  && !(defined(linux) || defined(__linux) || defined(__linux__))\
  && !defined(__ANDROID__)\
  && (!defined(__hpux) || defined(_REENTRANT)) \
  && (!defined(_AIX) || defined(__THREAD_SAFE))\
  && !defined(__wasm)
#define BOOST_FILESYSTEM_USE_READDIR_R
#endif

#else // BOOST_WINDOWS_API

#include <cwchar>
#include <windows.h>

#include "windows_tools.hpp"

#endif  // BOOST_WINDOWS_API

#include "error_handling.hpp"

//  BOOST_FILESYSTEM_STATUS_CACHE enables file_status cache in
//  dir_itr_increment. The config tests are placed here because some of the
//  macros being tested come from dirent.h.
//
// TODO: find out what macros indicate dirent::d_type present in more libraries
#if defined(BOOST_WINDOWS_API)\
  || defined(_DIRENT_HAVE_D_TYPE)// defined by GNU C library if d_type present
#define BOOST_FILESYSTEM_STATUS_CACHE
#endif

namespace fs = boost::filesystem;
using boost::system::error_code;
using boost::system::system_category;

namespace boost {
namespace filesystem {

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                 directory_entry                                      //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
file_status directory_entry::get_status(system::error_code* ec) const
{
  if (!status_known(m_status))
  {
    // optimization: if the symlink status is known, and it isn't a symlink,
    // then status and symlink_status are identical so just copy the
    // symlink status to the regular status.
    if (status_known(m_symlink_status) && !is_symlink(m_symlink_status))
    {
      m_status = m_symlink_status;
      if (ec != 0)
        ec->clear();
    }
    else
    {
      m_status = detail::status(m_path, ec);
    }
  }
  else if (ec != 0)
  {
    ec->clear();
  }

  return m_status;
}

BOOST_FILESYSTEM_DECL
file_status directory_entry::get_symlink_status(system::error_code* ec) const
{
  if (!status_known(m_symlink_status))
    m_symlink_status = detail::symlink_status(m_path, ec);
  else if (ec != 0)
    ec->clear();

  return m_symlink_status;
}

//  dispatch directory_entry supplied here rather than in
//  <boost/filesystem/path_traits.hpp>, thus avoiding header circularity.
//  test cases are in operations_unit_test.cpp

namespace path_traits {

void dispatch(const directory_entry& de,
#ifdef BOOST_WINDOWS_API
  std::wstring& to,
#else
  std::string& to,
#endif
  const codecvt_type&)
{
  to = de.path().native();
}

void dispatch(const directory_entry& de,
#ifdef BOOST_WINDOWS_API
  std::wstring& to
#else
  std::string& to
#endif
  )
{
  to = de.path().native();
}

} // namespace path_traits

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                               directory_iterator                                     //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace detail {

namespace {

#ifdef BOOST_POSIX_API

#if defined(BOOST_FILESYSTEM_USE_READDIR_R)

// Obtains maximum length of a path, not including the terminating zero
inline std::size_t get_path_max()
{
  // this code is based on Stevens and Rago, Advanced Programming in the
  // UNIX envirnment, 2nd Ed., ISBN 0-201-43307-9, page 49
  std::size_t max = 0;
  errno = 0;
  long res = ::pathconf("/", _PC_PATH_MAX);
  if (res < 0)
  {
#if defined(PATH_MAX)
    max = PATH_MAX;
#else
    max = 4096;
#endif
  }
  else
  {
    max = static_cast< std::size_t >(res); // relative root
#if defined(PATH_MAX)
    if (max < PATH_MAX)
      max = PATH_MAX;
#endif
  }

  if ((max + 1) < sizeof(dirent().d_name))
    max = sizeof(dirent().d_name) - 1;

  return max;
}

// Returns maximum length of a path, not including the terminating zero
inline std::size_t path_max()
{
  static const std::size_t max = get_path_max();
  return max;
}

#endif // BOOST_FILESYSTEM_USE_READDIR_R

error_code dir_itr_first(void*& handle, void*& buffer,
  const char* dir, std::string& target,
  fs::file_status&, fs::file_status&)
{
  if ((handle = ::opendir(dir)) == 0)
  {
    const int err = errno;
    return error_code(err, system_category());
  }
  target.assign(".");  // string was static but caused trouble
                       // when iteration called from dtor, after
                       // static had already been destroyed
  return error_code();
}

// *result set to NULL on end of directory
inline int readdir_r_simulator(DIR* dirp, void*& buffer, struct dirent** result)
{
#if defined(BOOST_FILESYSTEM_USE_READDIR_R)
  errno = 0;

  if (::sysconf(_SC_THREAD_SAFE_FUNCTIONS) >= 0)
  {
    struct dirent* storage = static_cast< struct dirent* >(buffer);
    if (BOOST_UNLIKELY(!storage))
    {
      // According to readdir description, there's no reliable way to predict the length of the d_name string.
      // It may exceed NAME_MAX and pathconf(_PC_NAME_MAX) limits. We are being conservative here and allocate
      // buffer that is enough for PATH_MAX as the directory name. Still, this doesn't guarantee there won't be
      // a buffer overrun. The readdir_r API is fundamentally flawed and we should avoid it as much as possible
      // in favor of readdir.
      const std::size_t name_size = path_max();
      const std::size_t buffer_size = (sizeof(dirent) - sizeof(dirent().d_name)) + name_size + 1; // + 1 for "\0"
      buffer = storage = static_cast< struct dirent* >(std::malloc(buffer_size));
      if (BOOST_UNLIKELY(!storage))
        return boost::system::errc::not_enough_memory;
      std::memset(storage, 0, buffer_size);
    }

    return ::readdir_r(dirp, storage, result);
  }
#endif

  errno = 0;

  struct dirent* p = ::readdir(dirp);
  *result = p;
  if (!p)
    return errno;
  return 0;
}

error_code dir_itr_increment(void*& handle, void*& buffer,
  std::string& target, fs::file_status& sf, fs::file_status& symlink_sf)
{
  dirent* result = NULL;
  int err = readdir_r_simulator(static_cast<DIR*>(handle), buffer, &result);
  if (BOOST_UNLIKELY(err != 0))
    return error_code(err, system_category());
  if (result == NULL)
    return fs::detail::dir_itr_close(handle, buffer);

  target = result->d_name;

#ifdef BOOST_FILESYSTEM_STATUS_CACHE
  if (result->d_type == DT_UNKNOWN) // filesystem does not supply d_type value
  {
    sf = symlink_sf = fs::file_status(fs::status_error);
  }
  else  // filesystem supplies d_type value
  {
    if (result->d_type == DT_DIR)
      sf = symlink_sf = fs::file_status(fs::directory_file);
    else if (result->d_type == DT_REG)
      sf = symlink_sf = fs::file_status(fs::regular_file);
    else if (result->d_type == DT_LNK)
    {
      sf = fs::file_status(fs::status_error);
      symlink_sf = fs::file_status(fs::symlink_file);
    }
    else
      sf = symlink_sf = fs::file_status(fs::status_error);
  }
#else
  sf = symlink_sf = fs::file_status(fs::status_error);
#endif
  return error_code();
}

#else // BOOST_WINDOWS_API

error_code dir_itr_first(void*& handle, const fs::path& dir,
  std::wstring& target, fs::file_status& sf, fs::file_status& symlink_sf)
// Note: an empty root directory has no "." or ".." entries, so this
// causes a ERROR_FILE_NOT_FOUND error which we do not considered an
// error. It is treated as eof instead.
{
  // use a form of search Sebastian Martel reports will work with Win98
  std::wstring dirpath(dir.wstring());
  dirpath += (dirpath.empty()
    || (dirpath[dirpath.size()-1] != L'\\'
      && dirpath[dirpath.size()-1] != L'/'
      && dirpath[dirpath.size()-1] != L':'))? L"\\*" : L"*";

  WIN32_FIND_DATAW data;
  if ((handle = ::FindFirstFileW(dirpath.c_str(), &data))
    == INVALID_HANDLE_VALUE)
  {
    handle = 0;  // signal eof
    DWORD error = ::GetLastError();
    return error_code( (error == ERROR_FILE_NOT_FOUND
                     // Windows Mobile returns ERROR_NO_MORE_FILES; see ticket #3551
                     || error == ERROR_NO_MORE_FILES)
      ? 0 : error, system_category() );
  }
  target = data.cFileName;
  if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
  // reparse points are complex, so don't try to handle them here; instead just mark
  // them as status_error which causes directory_entry caching to call status()
  // and symlink_status() which do handle reparse points fully
  {
    sf.type(fs::status_error);
    symlink_sf.type(fs::status_error);
  }
  else
  {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      sf.type(fs::directory_file);
      symlink_sf.type(fs::directory_file);
    }
    else
    {
      sf.type(fs::regular_file);
      symlink_sf.type(fs::regular_file);
    }
    sf.permissions(make_permissions(data.cFileName, data.dwFileAttributes));
    symlink_sf.permissions(sf.permissions());
  }
  return error_code();
}

error_code dir_itr_increment(void*& handle, std::wstring& target, fs::file_status& sf, fs::file_status& symlink_sf)
{
  WIN32_FIND_DATAW data;
  if (::FindNextFileW(handle, &data)== 0)// fails
  {
    DWORD error = ::GetLastError();
    fs::detail::dir_itr_close(handle);
    return error_code(error == ERROR_NO_MORE_FILES ? 0 : error, system_category());
  }
  target = data.cFileName;
  if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
  // reparse points are complex, so don't try to handle them here; instead just mark
  // them as status_error which causes directory_entry caching to call status()
  // and symlink_status() which do handle reparse points fully
  {
    sf.type(fs::status_error);
    symlink_sf.type(fs::status_error);
  }
  else
  {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      sf.type(fs::directory_file);
      symlink_sf.type(fs::directory_file);
    }
    else
    {
      sf.type(fs::regular_file);
      symlink_sf.type(fs::regular_file);
    }
    sf.permissions(make_permissions(data.cFileName, data.dwFileAttributes));
    symlink_sf.permissions(sf.permissions());
  }
  return error_code();
}
#endif

BOOST_CONSTEXPR_OR_CONST err_t not_found_error_code =
#ifdef BOOST_WINDOWS_API
  ERROR_PATH_NOT_FOUND
#else
  ENOENT
#endif
;

} // namespace

//  dir_itr_close is called both from the ~dir_itr_imp()destructor
//  and dir_itr_increment()
BOOST_FILESYSTEM_DECL
system::error_code dir_itr_close( // never throws
  void*& handle
#if defined(BOOST_POSIX_API)
  , void*& buffer
#endif
  ) BOOST_NOEXCEPT
{
#ifdef BOOST_POSIX_API

  if (buffer != NULL)
  {
    std::free(buffer);
    buffer = NULL;
  }

  if (handle != NULL)
  {
    DIR* h = static_cast<DIR*>(handle);
    handle = NULL;
    int err = 0;
    if (BOOST_UNLIKELY(::closedir(h) != 0))
    {
      err = errno;
      return error_code(err, system_category());
    }
  }

  return error_code();

#else

  if (handle != NULL)
  {
    ::FindClose(handle);
    handle = NULL;
  }
  return error_code();

#endif
}

BOOST_FILESYSTEM_DECL
void directory_iterator_construct(directory_iterator& it, const path& p, unsigned int opts, system::error_code* ec)
{
  if (error(p.empty() ? not_found_error_code : 0, p, ec,
            "boost::filesystem::directory_iterator::construct"))
  {
    return;
  }

  boost::intrusive_ptr< detail::dir_itr_imp > imp;
  if (!ec)
  {
    imp = new detail::dir_itr_imp();
  }
  else
  {
    imp = new (std::nothrow) detail::dir_itr_imp();
    if (BOOST_UNLIKELY(!imp))
    {
      *ec = make_error_code(system::errc::not_enough_memory);
      return;
    }
  }

  try
  {
    path::string_type filename;
    file_status file_stat, symlink_file_stat;
    error_code result = dir_itr_first(imp->handle,
#     if defined(BOOST_POSIX_API)
      imp->buffer,
#     endif
      p.c_str(), filename, file_stat, symlink_file_stat);

    if (result)
    {
      if (result != make_error_condition(system::errc::permission_denied) ||
        (opts & static_cast< unsigned int >(directory_options::skip_permission_denied)) == 0u)
      {
        error(result.value(), p,
          ec, "boost::filesystem::directory_iterator::construct");
      }

      return;
    }

    if (imp->handle)
    {
      // Not eof
      it.m_imp.swap(imp);
      it.m_imp->dir_entry.assign(p / filename, file_stat, symlink_file_stat);
      const path::string_type::value_type* filename_str = filename.c_str();
      if (filename_str[0] == path::dot // dot or dot-dot
        && (filename_str[1] == static_cast< path::string_type::value_type >('\0') ||
           (filename_str[1] == path::dot && filename_str[2] == static_cast< path::string_type::value_type >('\0'))))
      {
        detail::directory_iterator_increment(it, ec);
      }
    }
  }
  catch (std::bad_alloc&)
  {
    if (!ec)
      throw;

    *ec = make_error_code(boost::system::errc::not_enough_memory);
    it.m_imp.reset();
  }
}

BOOST_FILESYSTEM_DECL
void directory_iterator_increment(directory_iterator& it, system::error_code* ec)
{
  BOOST_ASSERT_MSG(!it.is_end(), "attempt to increment end iterator");

  if (ec)
    ec->clear();

  try
  {
    path::string_type filename;
    file_status file_stat, symlink_file_stat;
    system::error_code increment_ec;

    for (;;)
    {
      increment_ec = dir_itr_increment(it.m_imp->handle,
#       if defined(BOOST_POSIX_API)
        it.m_imp->buffer,
#       endif
        filename, file_stat, symlink_file_stat);

      if (BOOST_UNLIKELY(!!increment_ec))  // happens if filesystem is corrupt, such as on a damaged optical disc
      {
        boost::intrusive_ptr< detail::dir_itr_imp > imp;
        imp.swap(it.m_imp);
        path error_path(imp->dir_entry.path().parent_path());  // fix ticket #5900
        if (ec == NULL)
        {
          BOOST_FILESYSTEM_THROW(
            filesystem_error("boost::filesystem::directory_iterator::operator++",
              error_path,
              increment_ec));
        }
        *ec = increment_ec;
        return;
      }

      if (it.m_imp->handle == NULL)  // eof, make end
      {
        it.m_imp.reset();
        return;
      }

      const path::string_type::value_type* filename_str = filename.c_str();
      if (!(filename_str[0] == path::dot // !(dot or dot-dot)
        && (filename_str[1] == static_cast< path::string_type::value_type >('\0') ||
           (filename_str[1] == path::dot && filename_str[2] == static_cast< path::string_type::value_type >('\0')))))
      {
        it.m_imp->dir_entry.replace_filename(filename, file_stat, symlink_file_stat);
        return;
      }
    }
  }
  catch (std::bad_alloc&)
  {
    if (!ec)
      throw;

    it.m_imp.reset();
    *ec = make_error_code(boost::system::errc::not_enough_memory);
  }
}

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                           recursive_directory_iterator                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_construct(recursive_directory_iterator& it, const path& dir_path, unsigned int opts, system::error_code* ec)
{
  if (ec)
    ec->clear();

  directory_iterator dir_it;
  detail::directory_iterator_construct(dir_it, dir_path, opts, ec);
  if ((ec && *ec) || dir_it == directory_iterator())
    return;

  boost::intrusive_ptr< detail::recur_dir_itr_imp > imp;
  if (!ec)
  {
    imp = new detail::recur_dir_itr_imp(opts);
  }
  else
  {
    imp = new (std::nothrow) detail::recur_dir_itr_imp(opts);
    if (BOOST_UNLIKELY(!imp))
    {
      *ec = make_error_code(system::errc::not_enough_memory);
      return;
    }
  }

  try
  {
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    imp->m_stack.push_back(std::move(dir_it));
#else
    imp->m_stack.push_back(dir_it);
#endif

    it.m_imp.swap(imp);
  }
  catch (std::bad_alloc&)
  {
    if (ec)
    {
      *ec = make_error_code(system::errc::not_enough_memory);
      return;
    }

    throw;
  }
}

namespace {

void recursive_directory_iterator_pop_on_error(detail::recur_dir_itr_imp* imp)
{
  imp->m_stack.pop_back();

  while (!imp->m_stack.empty())
  {
    directory_iterator& dir_it = imp->m_stack.back();
    system::error_code increment_ec;
    detail::directory_iterator_increment(dir_it, &increment_ec);
    if (!increment_ec && dir_it != directory_iterator())
      break;

    imp->m_stack.pop_back();
  }
}

} // namespace

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_pop(recursive_directory_iterator& it, system::error_code* ec)
{
  BOOST_ASSERT_MSG(!it.is_end(), "pop() on end recursive_directory_iterator");
  detail::recur_dir_itr_imp* const imp = it.m_imp.get();

  if (ec)
    ec->clear();

  imp->m_stack.pop_back();

  while (true)
  {
    if (imp->m_stack.empty())
    {
      it.m_imp.reset(); // done, so make end iterator
      break;
    }

    directory_iterator& dir_it = imp->m_stack.back();
    system::error_code increment_ec;
    detail::directory_iterator_increment(dir_it, &increment_ec);
    if (BOOST_UNLIKELY(!!increment_ec))
    {
      if ((imp->m_options & static_cast< unsigned int >(directory_options::pop_on_error)) == 0u)
      {
        // Make an end iterator on errors
        it.m_imp.reset();
      }
      else
      {
        recursive_directory_iterator_pop_on_error(imp);

        if (imp->m_stack.empty())
          it.m_imp.reset(); // done, so make end iterator
      }

      if (ec == NULL)
      {
        BOOST_FILESYSTEM_THROW(
          filesystem_error("boost::filesystem::recursive_directory_iterator::pop", increment_ec));
      }

      *ec = increment_ec;
      return;
    }

    if (dir_it != directory_iterator())
      break;

    imp->m_stack.pop_back();
  }
}

namespace {

enum push_directory_result
{
  directory_not_pushed = 0u,
  directory_pushed = 1u,
  keep_depth = 1u << 1
};

// Returns: true if push occurs, otherwise false. Always returns false on error.
inline push_directory_result recursive_directory_iterator_push_directory(detail::recur_dir_itr_imp* imp, system::error_code& ec) BOOST_NOEXCEPT
{
  push_directory_result result = directory_not_pushed;
  try
  {
    //  Discover if the iterator is for a directory that needs to be recursed into,
    //  taking symlinks and options into account.

    if ((imp->m_options & static_cast< unsigned int >(directory_options::_detail_no_push)) != 0u)
    {
      imp->m_options &= ~static_cast< unsigned int >(directory_options::_detail_no_push);
      return result;
    }

    file_status symlink_stat;

    // if we are not recursing into symlinks, we are going to have to know if the
    // stack top is a symlink, so get symlink_status and verify no error occurred
    if ((imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink)) == 0u ||
      (imp->m_options & static_cast< unsigned int >(directory_options::skip_dangling_symlinks)) != 0u)
    {
      symlink_stat = imp->m_stack.back()->symlink_status(ec);
      if (ec)
        return result;
    }

    // Logic for following predicate was contributed by Daniel Aarno to handle cyclic
    // symlinks correctly and efficiently, fixing ticket #5652.
    //   if (((m_options & directory_options::follow_directory_symlink) == directory_options::follow_directory_symlink
    //         || !is_symlink(m_stack.back()->symlink_status()))
    //       && is_directory(m_stack.back()->status())) ...
    // The predicate code has since been rewritten to pass error_code arguments,
    // per ticket #5653.

    if ((imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink)) != 0u || !fs::is_symlink(symlink_stat))
    {
      file_status stat = imp->m_stack.back()->status(ec);
      if (BOOST_UNLIKELY(!!ec))
      {
        if (ec == make_error_condition(system::errc::no_such_file_or_directory) && fs::is_symlink(symlink_stat) &&
          (imp->m_options & static_cast< unsigned int >(directory_options::follow_directory_symlink | directory_options::skip_dangling_symlinks))
          == static_cast< unsigned int >(directory_options::follow_directory_symlink | directory_options::skip_dangling_symlinks))
        {
          // Skip dangling symlink and continue iteration on the current depth level
          ec = error_code();
        }

        return result;
      }

      if (!fs::is_directory(stat))
        return result;

      if (BOOST_UNLIKELY((imp->m_stack.size() - 1u) >= static_cast< std::size_t >((std::numeric_limits< int >::max)())))
      {
        // We cannot let depth to overflow
        ec = make_error_code(system::errc::value_too_large);
        // When depth overflow happens, avoid popping the current directory iterator
        // and attempt to continue iteration on the current depth.
        result = keep_depth;
        return result;
      }

      directory_iterator next(imp->m_stack.back()->path(), static_cast< BOOST_SCOPED_ENUM_NATIVE(directory_options) >(imp->m_options), ec);
      if (!ec && next != directory_iterator())
      {
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
        imp->m_stack.push_back(std::move(next)); // may throw
#else
        imp->m_stack.push_back(next); // may throw
#endif
        return directory_pushed;
      }
    }
  }
  catch (std::bad_alloc&)
  {
    ec = make_error_code(system::errc::not_enough_memory);
  }

  return result;
}

} // namespace

BOOST_FILESYSTEM_DECL
void recursive_directory_iterator_increment(recursive_directory_iterator& it, system::error_code* ec)
{
  BOOST_ASSERT_MSG(!it.is_end(), "increment() on end recursive_directory_iterator");
  detail::recur_dir_itr_imp* const imp = it.m_imp.get();

  if (ec)
    ec->clear();

  system::error_code local_ec;

  //  if various conditions are met, push a directory_iterator into the iterator stack
  push_directory_result push_result = recursive_directory_iterator_push_directory(imp, local_ec);
  if (push_result == directory_pushed)
    return;

  // report errors if any
  if (BOOST_UNLIKELY(!!local_ec))
  {
  on_error:
    if ((imp->m_options & static_cast< unsigned int >(directory_options::pop_on_error)) == 0u)
    {
      // Make an end iterator on errors
      it.m_imp.reset();
    }
    else
    {
      if ((push_result & keep_depth) != 0u)
      {
        system::error_code increment_ec;
        directory_iterator& dir_it = imp->m_stack.back();
        detail::directory_iterator_increment(dir_it, &increment_ec);
        if (!increment_ec && dir_it != directory_iterator())
          goto on_error_return;
      }

      recursive_directory_iterator_pop_on_error(imp);

      if (imp->m_stack.empty())
        it.m_imp.reset(); // done, so make end iterator
    }

  on_error_return:
    if (ec == NULL)
    {
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "filesystem::recursive_directory_iterator increment error",
        local_ec));
    }

    *ec = local_ec;
    return;
  }

  //  Do the actual increment operation on the top iterator in the iterator
  //  stack, popping the stack if necessary, until either the stack is empty or a
  //  non-end iterator is reached.
  while (true)
  {
    if (imp->m_stack.empty())
    {
      it.m_imp.reset(); // done, so make end iterator
      break;
    }

    directory_iterator& dir_it = imp->m_stack.back();
    detail::directory_iterator_increment(dir_it, &local_ec);
    if (BOOST_UNLIKELY(!!local_ec))
      goto on_error;

    if (dir_it != directory_iterator())
      break;

    imp->m_stack.pop_back();
  }
}

} // namespace detail

} // namespace filesystem
} // namespace boost
