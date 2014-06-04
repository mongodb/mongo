//  filesystem path.cpp  -------------------------------------------------------------  //

//  Copyright Beman Dawes 2008

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//  Old standard library configurations, particularly MingGW, don't support wide strings.
//  Report this with an explicit error message.
#include <boost/config.hpp>
# if defined( BOOST_NO_STD_WSTRING )
#   error Configuration not supported: Boost.Filesystem V3 and later requires std::wstring support
# endif

// define BOOST_FILESYSTEM_SOURCE so that <boost/system/config.hpp> knows
// the library is being built (possibly exporting rather than importing code)
#define BOOST_FILESYSTEM_SOURCE 

#ifndef BOOST_SYSTEM_NO_DEPRECATED 
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/scoped_array.hpp>
#include <boost/system/error_code.hpp>
#include <boost/assert.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cassert>

#ifdef BOOST_WINDOWS_API
# include "windows_file_codecvt.hpp"
# include <windows.h>
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
# include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#endif

#ifdef BOOST_FILESYSTEM_DEBUG
# include <iostream>
# include <iomanip>
#endif

namespace fs = boost::filesystem;

using boost::filesystem::path;

using std::string;
using std::wstring;

using boost::system::error_code;

#ifndef BOOST_FILESYSTEM_CODECVT_BUF_SIZE
# define BOOST_FILESYSTEM_CODECVT_BUF_SIZE 256
#endif

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                class path helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace
{
  //------------------------------------------------------------------------------------//
  //                        miscellaneous class path helpers                            //
  //------------------------------------------------------------------------------------//

  typedef path::value_type        value_type;
  typedef path::string_type       string_type;
  typedef string_type::size_type  size_type;

  const std::size_t default_codecvt_buf_size = BOOST_FILESYSTEM_CODECVT_BUF_SIZE;

# ifdef BOOST_WINDOWS_API

  const wchar_t separator = L'/';
  const wchar_t* const separators = L"/\\";
  const wchar_t* separator_string = L"/";
  const wchar_t* preferred_separator_string = L"\\";
  const wchar_t colon = L':';
  const wchar_t dot = L'.';
  const wchar_t questionmark = L'?';
  const fs::path dot_path(L".");
  const fs::path dot_dot_path(L"..");

  inline bool is_letter(wchar_t c)
  {
    return (c >= L'a' && c <=L'z') || (c >= L'A' && c <=L'Z');
  }

# else

  const char separator = '/';
  const char* const separators = "/";
  const char* separator_string = "/";
  const char* preferred_separator_string = "/";
  const char colon = ':';
  const char dot = '.';
  const fs::path dot_path(".");
  const fs::path dot_dot_path("..");

# endif

  inline bool is_separator(fs::path::value_type c)
  {
    return c == separator
#     ifdef BOOST_WINDOWS_API
      || c == path::preferred_separator
#     endif
      ;
  }

  bool is_root_separator(const string_type& str, size_type pos);
    // pos is position of the separator

  size_type filename_pos(const string_type& str,
                          size_type end_pos); // end_pos is past-the-end position
  //  Returns: 0 if str itself is filename (or empty)

  size_type root_directory_start(const string_type& path, size_type size);
  //  Returns:  npos if no root_directory found

  void first_element(
      const string_type& src,
      size_type& element_pos,
      size_type& element_size,
#     if !BOOST_WORKAROUND(BOOST_MSVC, <= 1310) // VC++ 7.1
      size_type size = string_type::npos
#     else
      size_type size = -1
#     endif
    );

}  // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            class path implementation                                 //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace boost
{
namespace filesystem
{
  path& path::operator/=(const path& p)
  {
    if (p.empty())
      return *this;
    if (this == &p)  // self-append
    {
      path rhs(p);
      if (!is_separator(rhs.m_pathname[0]))
        m_append_separator_if_needed();
      m_pathname += rhs.m_pathname;
    }
    else
    {
      if (!is_separator(*p.m_pathname.begin()))
        m_append_separator_if_needed();
      m_pathname += p.m_pathname;
    }
    return *this;
  }

  path& path::operator/=(const value_type* ptr)
  {
    if (!*ptr)
      return *this;
    if (ptr >= m_pathname.data()
      && ptr < m_pathname.data() + m_pathname.size())  // overlapping source
    {
      path rhs(ptr);
      if (!is_separator(rhs.m_pathname[0]))
        m_append_separator_if_needed();
      m_pathname += rhs.m_pathname;
    }
    else
    {
      if (!is_separator(*ptr))
        m_append_separator_if_needed();
      m_pathname += ptr;
    }
    return *this;
  }

  int path::compare(const path& p) const BOOST_NOEXCEPT
  {
    return detail::lex_compare(begin(), end(), p.begin(), p.end());
  }

# ifdef BOOST_WINDOWS_API

  const std::string path::generic_string(const codecvt_type& cvt) const
  { 
    path tmp(*this);
    std::replace(tmp.m_pathname.begin(), tmp.m_pathname.end(), L'\\', L'/');
    return tmp.string(cvt);
  }

  const std::wstring path::generic_wstring() const
  { 
    path tmp(*this);
    std::replace(tmp.m_pathname.begin(), tmp.m_pathname.end(), L'\\', L'/');
    return tmp.wstring();
  }

# endif  // BOOST_WINDOWS_API

  //  m_append_separator_if_needed  ----------------------------------------------------//

  path::string_type::size_type path::m_append_separator_if_needed()
  {
    if (!m_pathname.empty() &&
#     ifdef BOOST_WINDOWS_API
      *(m_pathname.end()-1) != colon && 
#     endif
      !is_separator(*(m_pathname.end()-1)))
    {
      string_type::size_type tmp(m_pathname.size());
      m_pathname += preferred_separator;
      return tmp;
    }
    return 0;
  }

  //  m_erase_redundant_separator  -----------------------------------------------------//

  void path::m_erase_redundant_separator(string_type::size_type sep_pos)
  {
    if (sep_pos                         // a separator was added
      && sep_pos < m_pathname.size()         // and something was appended
      && (m_pathname[sep_pos+1] == separator // and it was also separator
#      ifdef BOOST_WINDOWS_API
       || m_pathname[sep_pos+1] == preferred_separator  // or preferred_separator
#      endif
)) { m_pathname.erase(sep_pos, 1); } // erase the added separator
  }

  //  modifiers  -----------------------------------------------------------------------//

# ifdef BOOST_WINDOWS_API
  path & path::make_preferred()
  {
    std::replace(m_pathname.begin(), m_pathname.end(), L'/', L'\\');
    return *this;
  }
# endif

  path& path::remove_filename()
  {
    m_pathname.erase(m_parent_path_end());
    return *this;
  }

  path& path::replace_extension(const path& new_extension)
  {
    // erase existing extension, including the dot, if any
    m_pathname.erase(m_pathname.size()-extension().m_pathname.size());

    if (!new_extension.empty())
    {
      // append new_extension, adding the dot if necessary
      if (new_extension.m_pathname[0] != dot)
        m_pathname.push_back(dot);
      m_pathname.append(new_extension.m_pathname);
    }

    return *this;
  }

  //  decomposition  -------------------------------------------------------------------//

  path  path::root_path() const
  { 
    path temp(root_name());
    if (!root_directory().empty()) temp.m_pathname += root_directory().c_str();
    return temp;
  } 

  path path::root_name() const
  {
    iterator itr(begin());

    return (itr.m_pos != m_pathname.size()
      && (
          (itr.m_element.m_pathname.size() > 1
            && is_separator(itr.m_element.m_pathname[0])
            && is_separator(itr.m_element.m_pathname[1])
   )
#       ifdef BOOST_WINDOWS_API
        || itr.m_element.m_pathname[itr.m_element.m_pathname.size()-1] == colon
#       endif
  ))
      ? itr.m_element
      : path();
  }

  path path::root_directory() const
  {
    size_type pos(root_directory_start(m_pathname, m_pathname.size()));

    return pos == string_type::npos
      ? path()
      : path(m_pathname.c_str() + pos, m_pathname.c_str() + pos + 1);
  }

  path path::relative_path() const
  {
    iterator itr(begin());

    for (; itr.m_pos != m_pathname.size()
      && (is_separator(itr.m_element.m_pathname[0])
#     ifdef BOOST_WINDOWS_API
      || itr.m_element.m_pathname[itr.m_element.m_pathname.size()-1] == colon
#     endif
    ); ++itr) {}

    return path(m_pathname.c_str() + itr.m_pos);
  }

  string_type::size_type path::m_parent_path_end() const
  {
    size_type end_pos(filename_pos(m_pathname, m_pathname.size()));

    bool filename_was_separator(m_pathname.size()
      && is_separator(m_pathname[end_pos]));

    // skip separators unless root directory
    size_type root_dir_pos(root_directory_start(m_pathname, end_pos));
    for (; 
      end_pos > 0
      && (end_pos-1) != root_dir_pos
      && is_separator(m_pathname[end_pos-1])
      ;
      --end_pos) {}

   return (end_pos == 1 && root_dir_pos == 0 && filename_was_separator)
     ? string_type::npos
     : end_pos;
  }

  path path::parent_path() const
  {
   size_type end_pos(m_parent_path_end());
   return end_pos == string_type::npos
     ? path()
     : path(m_pathname.c_str(), m_pathname.c_str() + end_pos);
  }

  path path::filename() const
  {
    size_type pos(filename_pos(m_pathname, m_pathname.size()));
    return (m_pathname.size()
              && pos
              && is_separator(m_pathname[pos])
              && !is_root_separator(m_pathname, pos))
      ? dot_path
      : path(m_pathname.c_str() + pos);
  }

  path path::stem() const
  {
    path name(filename());
    if (name == dot_path || name == dot_dot_path) return name;
    size_type pos(name.m_pathname.rfind(dot));
    return pos == string_type::npos
      ? name
      : path(name.m_pathname.c_str(), name.m_pathname.c_str() + pos);
  }

  path path::extension() const
  {
    path name(filename());
    if (name == dot_path || name == dot_dot_path) return path();
    size_type pos(name.m_pathname.rfind(dot));
    return pos == string_type::npos
      ? path()
      : path(name.m_pathname.c_str() + pos);
  }

  // m_normalize  ----------------------------------------------------------------------//

  path& path::m_normalize()
  {
    if (m_pathname.empty()) return *this;
      
    path temp;
    iterator start(begin());
    iterator last(end());
    iterator stop(last--);
    for (iterator itr(start); itr != stop; ++itr)
    {
      // ignore "." except at start and last
      if (itr->native().size() == 1
        && (itr->native())[0] == dot
        && itr != start
        && itr != last) continue;

      // ignore a name and following ".."
      if (!temp.empty()
        && itr->native().size() == 2
        && (itr->native())[0] == dot
        && (itr->native())[1] == dot) // dot dot
      {
        string_type lf(temp.filename().native());  
        if (lf.size() > 0  
          && (lf.size() != 1
            || (lf[0] != dot
              && lf[0] != separator))
          && (lf.size() != 2 
            || (lf[0] != dot
              && lf[1] != dot
#             ifdef BOOST_WINDOWS_API
              && lf[1] != colon
#             endif
               )
             )
          )
        {
          temp.remove_filename();
          // if not root directory, must also remove "/" if any
          if (temp.m_pathname.size() > 0
            && temp.m_pathname[temp.m_pathname.size()-1]
              == separator)
          {
            string_type::size_type rds(
              root_directory_start(temp.m_pathname, temp.m_pathname.size()));
            if (rds == string_type::npos
              || rds != temp.m_pathname.size()-1) 
              { temp.m_pathname.erase(temp.m_pathname.size()-1); }
          }

          iterator next(itr);
          if (temp.empty() && ++next != stop
            && next == last && *last == dot_path) temp /= dot_path;
          continue;
        }
      }

      temp /= *itr;
    };

    if (temp.empty()) temp /= dot_path;
    m_pathname = temp.m_pathname;
    return *this;
  }

}  // namespace filesystem
}  // namespace boost
  
//--------------------------------------------------------------------------------------//
//                                                                                      //
//                         class path helpers implementation                            //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace
{

  //  is_root_separator  ---------------------------------------------------------------//

  bool is_root_separator(const string_type & str, size_type pos)
    // pos is position of the separator
  {
    BOOST_ASSERT_MSG(!str.empty() && is_separator(str[pos]),
      "precondition violation");

    // subsequent logic expects pos to be for leftmost slash of a set
    while (pos > 0 && is_separator(str[pos-1]))
      --pos;

    //  "/" [...]
    if (pos == 0)  
      return true;

# ifdef BOOST_WINDOWS_API
    //  "c:/" [...]
    if (pos == 2 && is_letter(str[0]) && str[1] == colon)  
      return true;
# endif

    //  "//" name "/"
    if (pos < 3 || !is_separator(str[0]) || !is_separator(str[1]))
      return false;

    return str.find_first_of(separators, 2) == pos;
  }

  //  filename_pos  --------------------------------------------------------------------//

  size_type filename_pos(const string_type & str,
                          size_type end_pos) // end_pos is past-the-end position
    // return 0 if str itself is filename (or empty)
  {
    // case: "//"
    if (end_pos == 2 
      && is_separator(str[0])
      && is_separator(str[1])) return 0;

    // case: ends in "/"
    if (end_pos && is_separator(str[end_pos-1]))
      return end_pos-1;
    
    // set pos to start of last element
    size_type pos(str.find_last_of(separators, end_pos-1));

#   ifdef BOOST_WINDOWS_API
    if (pos == string_type::npos)
      pos = str.find_last_of(colon, end_pos-2);
#   endif

    return (pos == string_type::npos // path itself must be a filename (or empty)
      || (pos == 1 && is_separator(str[0]))) // or net
        ? 0 // so filename is entire string
        : pos + 1; // or starts after delimiter
  }

  //  root_directory_start  ------------------------------------------------------------//

  size_type root_directory_start(const string_type & path, size_type size)
  // return npos if no root_directory found
  {

#   ifdef BOOST_WINDOWS_API
    // case "c:/"
    if (size > 2
      && path[1] == colon
      && is_separator(path[2])) return 2;
#   endif

    // case "//"
    if (size == 2
      && is_separator(path[0])
      && is_separator(path[1])) return string_type::npos;

#   ifdef BOOST_WINDOWS_API
    // case "\\?\"
    if (size > 4
      && is_separator(path[0])
      && is_separator(path[1])
      && path[2] == questionmark
      && is_separator(path[3]))
    {
      string_type::size_type pos(path.find_first_of(separators, 4));
        return pos < size ? pos : string_type::npos;
    }
#   endif

    // case "//net {/}"
    if (size > 3
      && is_separator(path[0])
      && is_separator(path[1])
      && !is_separator(path[2]))
    {
      string_type::size_type pos(path.find_first_of(separators, 2));
      return pos < size ? pos : string_type::npos;
    }
    
    // case "/"
    if (size > 0 && is_separator(path[0])) return 0;

    return string_type::npos;
  }

  //  first_element --------------------------------------------------------------------//
  //   sets pos and len of first element, excluding extra separators
  //   if src.empty(), sets pos,len, to 0,0.

  void first_element(
      const string_type & src,
      size_type & element_pos,
      size_type & element_size,
      size_type size
)
  {
    if (size == string_type::npos) size = src.size();
    element_pos = 0;
    element_size = 0;
    if (src.empty()) return;

    string_type::size_type cur(0);
    
    // deal with // [network]
    if (size >= 2 && is_separator(src[0])
      && is_separator(src[1])
      && (size == 2
        || !is_separator(src[2])))
    { 
      cur += 2;
      element_size += 2;
    }

    // leading (not non-network) separator
    else if (is_separator(src[0]))
    {
      ++element_size;
      // bypass extra leading separators
      while (cur+1 < size
        && is_separator(src[cur+1]))
      {
        ++cur;
        ++element_pos;
      }
      return;
    }

    // at this point, we have either a plain name, a network name,
    // or (on Windows only) a device name

    // find the end
    while (cur < size
#     ifdef BOOST_WINDOWS_API
      && src[cur] != colon
#     endif
      && !is_separator(src[cur]))
    {
      ++cur;
      ++element_size;
    }

#   ifdef BOOST_WINDOWS_API
    if (cur == size) return;
    // include device delimiter
    if (src[cur] == colon)
      { ++element_size; }
#   endif

    return;
  }

}  // unnamed namespace


namespace boost
{
namespace filesystem
{
  namespace detail
  {
    BOOST_FILESYSTEM_DECL
      int lex_compare(path::iterator first1, path::iterator last1,
        path::iterator first2, path::iterator last2)
    {
      for (; first1 != last1 && first2 != last2;)
      {
        if (first1->native() < first2->native()) return -1;
        if (first2->native() < first1->native()) return 1;
        BOOST_ASSERT(first2->native() == first1->native());
        ++first1;
        ++first2;
      }
      if (first1 == last1 && first2 == last2)
        return 0;
      return first1 == last1 ? -1 : 1;
    }
  }

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        class path::iterator implementation                           //
//                                                                                      //
//--------------------------------------------------------------------------------------//

  path::iterator path::begin() const
  {
    iterator itr;
    itr.m_path_ptr = this;
    size_type element_size;
    first_element(m_pathname, itr.m_pos, element_size);
    itr.m_element = m_pathname.substr(itr.m_pos, element_size);
    if (itr.m_element.m_pathname == preferred_separator_string)
      itr.m_element.m_pathname = separator_string;  // needed for Windows, harmless on POSIX
    return itr;
  }

  path::iterator path::end() const
  {
    iterator itr;
    itr.m_path_ptr = this;
    itr.m_pos = m_pathname.size();
    return itr;
  }

  void path::m_path_iterator_increment(path::iterator & it)
  {
    BOOST_ASSERT_MSG(it.m_pos < it.m_path_ptr->m_pathname.size(),
      "path::basic_iterator increment past end()");

    // increment to position past current element; if current element is implicit dot,
    // this will cause it.m_pos to represent the end iterator
    it.m_pos += it.m_element.m_pathname.size();

    // if the end is reached, we are done
    if (it.m_pos == it.m_path_ptr->m_pathname.size())
    {
      it.m_element.clear();  // aids debugging, may release unneeded memory
      return;
    }

    // both POSIX and Windows treat paths that begin with exactly two separators specially
    bool was_net(it.m_element.m_pathname.size() > 2
      && is_separator(it.m_element.m_pathname[0])
      && is_separator(it.m_element.m_pathname[1])
      && !is_separator(it.m_element.m_pathname[2]));

    // process separator (Windows drive spec is only case not a separator)
    if (is_separator(it.m_path_ptr->m_pathname[it.m_pos]))
    {
      // detect root directory
      if (was_net
#       ifdef BOOST_WINDOWS_API
        // case "c:/"
        || it.m_element.m_pathname[it.m_element.m_pathname.size()-1] == colon
#       endif
         )
      {
        it.m_element.m_pathname = separator;  // generic format; see docs
        return;
      }

      // skip separators until it.m_pos points to the start of the next element
      while (it.m_pos != it.m_path_ptr->m_pathname.size()
        && is_separator(it.m_path_ptr->m_pathname[it.m_pos]))
        { ++it.m_pos; }

      // detect trailing separator, and treat it as ".", per POSIX spec
      if (it.m_pos == it.m_path_ptr->m_pathname.size()
        && !is_root_separator(it.m_path_ptr->m_pathname, it.m_pos-1)) 
      {
        --it.m_pos;
        it.m_element = dot_path;
        return;
      }
    }

    // get m_element
    size_type end_pos(it.m_path_ptr->m_pathname.find_first_of(separators, it.m_pos));
    if (end_pos == string_type::npos)
      end_pos = it.m_path_ptr->m_pathname.size();
    it.m_element = it.m_path_ptr->m_pathname.substr(it.m_pos, end_pos - it.m_pos);
  }

  void path::m_path_iterator_decrement(path::iterator & it)
  {
    BOOST_ASSERT_MSG(it.m_pos, "path::iterator decrement past begin()");

    size_type end_pos(it.m_pos);

    // if at end and there was a trailing non-root '/', return "."
    if (it.m_pos == it.m_path_ptr->m_pathname.size()
      && it.m_path_ptr->m_pathname.size() > 1
      && is_separator(it.m_path_ptr->m_pathname[it.m_pos-1])
      && !is_root_separator(it.m_path_ptr->m_pathname, it.m_pos-1) 
       )
    {
      --it.m_pos;
      it.m_element = dot_path;
      return;
    }

    size_type root_dir_pos(root_directory_start(it.m_path_ptr->m_pathname, end_pos));

    // skip separators unless root directory
    for (
      ; 
      end_pos > 0
      && (end_pos-1) != root_dir_pos
      && is_separator(it.m_path_ptr->m_pathname[end_pos-1])
      ;
      --end_pos) {}

    it.m_pos = filename_pos(it.m_path_ptr->m_pathname, end_pos);
    it.m_element = it.m_path_ptr->m_pathname.substr(it.m_pos, end_pos - it.m_pos);
    if (it.m_element.m_pathname == preferred_separator_string) // needed for Windows, harmless on POSIX 
      it.m_element.m_pathname = separator_string;    // generic format; see docs 
  }

}  // namespace filesystem
}  // namespace boost

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                                 detail helpers                                       //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace
{

  //------------------------------------------------------------------------------------//
  //                              locale helpers                                        //
  //------------------------------------------------------------------------------------//

#if defined(BOOST_WINDOWS_API) && defined(BOOST_FILESYSTEM_STATIC_LINK)

  inline std::locale default_locale()
  {
    std::locale global_loc = std::locale();
    std::locale loc(global_loc, new windows_file_codecvt);
    return loc;
  }

  inline std::locale& path_locale()
  {
    static std::locale loc(default_locale());
    return loc;
  }

  inline const path::codecvt_type*& codecvt_facet_ptr()
  {
    static const std::codecvt<wchar_t, char, std::mbstate_t>*
     facet(
       &std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >
        (path_locale()));
    return facet;
  }

#elif defined(BOOST_WINDOWS_API) && !defined(BOOST_FILESYSTEM_STATIC_LINK)

  std::locale path_locale(std::locale(), new windows_file_codecvt); 

  const std::codecvt<wchar_t, char, std::mbstate_t>*
    codecvt_facet_ptr(&std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >
      (path_locale));

#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)

  // "All BSD system functions expect their string parameters to be in UTF-8 encoding
  // and nothing else." See
  // http://developer.apple.com/mac/library/documentation/MacOSX/Conceptual/BPInternational/Articles/FileEncodings.html
  //
  // "The kernel will reject any filename that is not a valid UTF-8 string, and it will
  // even be normalized (to Unicode NFD) before stored on disk, at least when using HFS.
  // The right way to deal with it would be to always convert the filename to UTF-8
  // before trying to open/create a file." See
  // http://lists.apple.com/archives/unix-porting/2007/Sep/msg00023.html
  //
  // "How a file name looks at the API level depends on the API. Current Carbon APIs
  // handle file names as an array of UTF-16 characters; POSIX ones handle them as an
  // array of UTF-8, which is why UTF-8 works well in Terminal. How it's stored on disk
  // depends on the disk format; HFS+ uses UTF-16, but that's not important in most
  // cases." See
  // http://lists.apple.com/archives/applescript-users/2002/Sep/msg00319.html
  //
  // Many thanks to Peter Dimov for digging out the above references!

  std::locale path_locale(std::locale(),
                          new boost::filesystem::detail::utf8_codecvt_facet);

  const std::codecvt<wchar_t, char, std::mbstate_t>*
    codecvt_facet_ptr(&std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >
      (path_locale));

#else  // Other POSIX

  // ISO C calls std::locale("") "the locale-specific native environment", and this
  // locale is the default for many POSIX-based operating systems such as Linux.

  // std::locale("") construction can throw (if environmental variables LC_MESSAGES or
  // or LANG are wrong, for example), so lazy initialization is used to ensure
  // that exceptions occur after main() starts and so can be caught.

  std::locale path_locale;  // initialized by path::codecvt() below
  const std::codecvt<wchar_t, char, std::mbstate_t>* codecvt_facet_ptr;  // ditto

# endif

}  // unnamed namespace

//--------------------------------------------------------------------------------------//
//                           path::imbue implementation                                 //
//--------------------------------------------------------------------------------------//

namespace boost
{
namespace filesystem
{

#if defined(BOOST_WINDOWS_API) && defined(BOOST_FILESYSTEM_STATIC_LINK)

  const path::codecvt_type& path::codecvt()
  {
    BOOST_ASSERT_MSG(codecvt_facet_ptr(), "codecvt_facet_ptr() facet hasn't been properly initialized");
    return *codecvt_facet_ptr();
  }

  std::locale path::imbue(const std::locale & loc)
  {
    std::locale temp(path_locale());
    path_locale() = loc;
    codecvt_facet_ptr() =
      &std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >(path_locale());
    return temp;
  }

#else

  const path::codecvt_type& path::codecvt()
  {
#   if defined(BOOST_POSIX_API) && \
      !(defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
      // A local static initialized by calling path::imbue ensures that std::locale(""),
      // which may throw, is called only if path_locale and condecvt_facet will actually
      // be used. Thus misconfigured environmental variables will only cause an
      // exception if a valid std::locale("") is actually needed.
      static std::locale posix_lazy_initialization(path::imbue(std::locale("")));
#   endif
    return *codecvt_facet_ptr;
  }

  std::locale path::imbue(const std::locale& loc)
  {
    std::locale temp(path_locale);
    path_locale = loc;
    codecvt_facet_ptr =
      &std::use_facet<std::codecvt<wchar_t, char, std::mbstate_t> >(path_locale);
    return temp;
  }


#endif

}  // namespace filesystem
}  // namespace boost
