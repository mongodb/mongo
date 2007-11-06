//  boost/filesystem/operations.hpp  -----------------------------------------//

//  Copyright 2002-2005 Beman Dawes
//  Copyright 2002 Jan Langer
//  Copyright 2001 Dietmar Kuehl                                        
//  
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy
//  at http://www.boost.org/LICENSE_1_0.txt)                             

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------// 

#ifndef BOOST_FILESYSTEM_OPERATIONS_HPP
#define BOOST_FILESYSTEM_OPERATIONS_HPP

#include <boost/filesystem/path.hpp>  // includes <boost/filesystem/config.hpp>

#include <boost/shared_ptr.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/iterator.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>

#include <string>
#include <utility> // for pair
#include <ctime>

#ifdef BOOST_WINDOWS_API
#  include <fstream>
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT >= 0x0500
#    define BOOST_FS_HARD_LINK // Default for Windows 2K or later 
#  endif
#endif

#include <boost/config/abi_prefix.hpp> // must be the last #include

# ifdef BOOST_NO_STDC_NAMESPACE
    namespace std { using ::time_t; }
# endif

# ifndef BOOST_FILESYSTEM_NARROW_ONLY
#   define BOOST_FS_FUNC(BOOST_FS_TYPE) \
      template<class Path> typename boost::enable_if<is_basic_path<Path>, \
      BOOST_FS_TYPE>::type
#   define BOOST_INLINE_FS_FUNC(BOOST_FS_TYPE) \
      template<class Path> inline typename boost::enable_if<is_basic_path<Path>, \
      BOOST_FS_TYPE>::type
#   define BOOST_FS_TYPENAME typename
# else
#   define BOOST_FS_FUNC(BOOST_FS_TYPE) inline BOOST_FS_TYPE
#   define BOOST_INLINE_FS_FUNC(BOOST_FS_TYPE) inline BOOST_FS_TYPE
    typedef boost::filesystem::path Path;
#   define BOOST_FS_TYPENAME
# endif

//----------------------------------------------------------------------------//

namespace boost
{
  namespace filesystem
  {
    template<class Path> class basic_directory_iterator;

    // BOOST_FILESYSTEM_NARROW_ONLY needs this:
    typedef basic_directory_iterator<path> directory_iterator;

    template<class Path> class basic_directory_entry;

    enum file_type
    { 
      status_unknown,
      file_not_found,
      regular_file,
      directory_file,
      // the following will never be reported by some operating or file systems
      symlink_file,
      block_file,
      character_file,
      fifo_file,
      socket_file,
      type_unknown // file does exist, but isn't one of the above types
    };

    class file_status
    {
    public:
      explicit file_status( file_type v = status_unknown ) : m_value(v) {}

      void type( file_type v )  { m_value = v; }
      file_type type() const    { return m_value; }

    private:
      // the internal representation is unspecified so that additional state
      // information such as permissions can be added in the future; this
      // implementation just uses status_type as the internal representation

      file_type m_value;
    };

    inline bool status_known( file_status f ) { return f.type() != status_unknown; }
    inline bool exists( file_status f )       { return f.type() != status_unknown && f.type() != file_not_found; }
    inline bool is_regular( file_status f )   { return f.type() == regular_file; }
    inline bool is_directory( file_status f ) { return f.type() == directory_file; }
    inline bool is_symlink( file_status f )   { return f.type() == symlink_file; }
    inline bool is_other( file_status f )     { return exists(f) && !is_regular(f) && !is_directory(f) && !is_symlink(f); }

    struct space_info
    {
      // all values are byte counts
      boost::uintmax_t capacity;
      boost::uintmax_t free;      // <= capacity
      boost::uintmax_t available; // <= free
    };

    namespace detail
    {
      typedef std::pair< boost::filesystem::system_error_type, bool >
        query_pair;

      typedef std::pair< boost::filesystem::system_error_type, boost::uintmax_t >
        uintmax_pair;

      typedef std::pair< boost::filesystem::system_error_type, std::time_t >
        time_pair;

      typedef std::pair< boost::filesystem::system_error_type, space_info >
        space_pair;

      template< class Path >
      struct directory_pair
      {
        typedef std::pair< boost::filesystem::system_error_type,
          typename Path::external_string_type > type;
      };

#   ifndef BOOST_FILESYSTEM_NO_DEPRECATED
      BOOST_FILESYSTEM_DECL bool
        symbolic_link_exists_api( const std::string & ); // deprecated
#   endif

      BOOST_FILESYSTEM_DECL file_status
        status_api( const std::string & ph, system_error_type & ec );
#   ifndef BOOST_WINDOWS_API
      BOOST_FILESYSTEM_DECL file_status
        symlink_status_api( const std::string & ph, system_error_type & ec );
#   endif
      BOOST_FILESYSTEM_DECL query_pair
        is_empty_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL query_pair
        equivalent_api( const std::string & ph1, const std::string & ph2 );
      BOOST_FILESYSTEM_DECL uintmax_pair
        file_size_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL space_pair
        space_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL time_pair 
        last_write_time_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        last_write_time_api( const std::string & ph, std::time_t new_value );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        get_current_path_api( std::string & ph );
      BOOST_FILESYSTEM_DECL query_pair
        create_directory_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        create_hard_link_api( const std::string & to_ph,
          const std::string & from_ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        create_symlink_api( const std::string & to_ph,
          const std::string & from_ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        remove_api( const std::string & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        rename_api( const std::string & from, const std::string & to );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        copy_file_api( const std::string & from, const std::string & to );

#   if defined(BOOST_WINDOWS_API)
      
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        get_full_path_name_api( const std::string & ph, std::string & target );

#     if !defined(BOOST_FILESYSTEM_NARROW_ONLY)

      BOOST_FILESYSTEM_DECL  boost::filesystem::file_status
        status_api( const std::wstring & ph, system_error_type & ec );
      BOOST_FILESYSTEM_DECL query_pair 
        is_empty_api( const std::wstring & ph );
      BOOST_FILESYSTEM_DECL query_pair
        equivalent_api( const std::wstring & ph1, const std::wstring & ph2 );
      BOOST_FILESYSTEM_DECL uintmax_pair 
        file_size_api( const std::wstring & ph );
      BOOST_FILESYSTEM_DECL space_pair 
        space_api( const std::wstring & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        get_full_path_name_api( const std::wstring & ph, std::wstring & target );
      BOOST_FILESYSTEM_DECL time_pair 
        last_write_time_api( const std::wstring & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        last_write_time_api( const std::wstring & ph, std::time_t new_value );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type 
        get_current_path_api( std::wstring & ph );
      BOOST_FILESYSTEM_DECL query_pair
        create_directory_api( const std::wstring & ph );
# ifdef BOOST_FS_HARD_LINK
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        create_hard_link_api( const std::wstring & existing_ph,
          const std::wstring & new_ph );
# endif
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        create_symlink_api( const std::wstring & to_ph,
          const std::wstring & from_ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        remove_api( const std::wstring & ph );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        rename_api( const std::wstring & from, const std::wstring & to );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        copy_file_api( const std::wstring & from, const std::wstring & to );

#     endif
#   endif

      template<class Path>
      unsigned long remove_all_aux( const Path & ph );

    } // namespace detail

//  operations functions  ----------------------------------------------------//

    //  The non-template overloads enable automatic conversion from std and
    //  C-style strings. See basic_path constructors. The enable_if for the
    //  templates implements the famous "do-the-right-thing" rule.

//  query functions  ---------------------------------------------------------//

    BOOST_INLINE_FS_FUNC(file_status)
    status( const Path & ph, system_error_type & ec )
      { return detail::status_api( ph.external_file_string(), ec ); }

    BOOST_FS_FUNC(file_status)
    status( const Path & ph )
    { 
      system_error_type ec;
      file_status result( detail::status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
        "boost::filesystem::status", ph, ec ) );
      return result;
    }

    BOOST_INLINE_FS_FUNC(file_status)
    symlink_status( const Path & ph, system_error_type & ec )
#   ifdef BOOST_WINDOWS_API
      { return detail::status_api( ph.external_file_string(), ec ); }
#   else
      { return detail::symlink_status_api( ph.external_file_string(), ec ); }
#   endif

    BOOST_FS_FUNC(file_status)
    symlink_status( const Path & ph )
    { 
      system_error_type ec;
      file_status result( symlink_status( ph, ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
        "boost::filesystem::symlink_status", ph, ec ) );
      return result;
    }

# ifndef BOOST_FILESYSTEM_NO_DEPRECATED
    inline bool symbolic_link_exists( const path & ph )
      { return is_symlink( symlink_status(ph) ); }
#endif

    BOOST_FS_FUNC(bool) exists( const Path & ph )
    { 
      system_error_type ec;
      file_status result( detail::status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::exists", ph, ec ) );
      return exists( result );
    }

    BOOST_FS_FUNC(bool) is_directory( const Path & ph )
    { 
      system_error_type ec;
      file_status result( detail::status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::is_directory", ph, ec ) );
      return is_directory( result );
    }

    BOOST_FS_FUNC(bool) is_regular( const Path & ph )
    { 
      system_error_type ec;
      file_status result( detail::status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::is_regular", ph, ec ) );
      return is_regular( result );
    }

    BOOST_FS_FUNC(bool) is_other( const Path & ph )
    { 
      system_error_type ec;
      file_status result( detail::status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::is_other", ph, ec ) );
      return is_other( result );
    }

    BOOST_FS_FUNC(bool) is_symlink(
#   ifdef BOOST_WINDOWS_API
      const Path & )
    {
      return false;
#   else
      const Path & ph)
    {
      system_error_type ec;
      file_status result( detail::symlink_status_api( ph.external_file_string(), ec ) );
      if ( ec )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::is_symlink", ph, ec ) );
      return is_symlink( result );
#   endif
    }

    // VC++ 7.0 and earlier has a serious namespace bug that causes a clash
    // between boost::filesystem::is_empty and the unrelated type trait
    // boost::is_empty.

# if !defined( BOOST_MSVC ) || BOOST_MSVC > 1300
    BOOST_FS_FUNC(bool) is_empty( const Path & ph )
# else
    BOOST_FS_FUNC(bool) _is_empty( const Path & ph )
# endif
    {
      detail::query_pair result = detail::is_empty_api( ph.external_file_string() );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::is_empty", ph, result.first ) );
      return result.second;
    }

    BOOST_FS_FUNC(bool) equivalent( const Path & ph1, const Path & ph2 )
    {
      detail::query_pair result = detail::equivalent_api(
        ph1.external_file_string(), ph2.external_file_string() );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::equivalent", ph1, ph2, result.first ) );
      return result.second;
    }

    BOOST_FS_FUNC(boost::uintmax_t) file_size( const Path & ph )
    {
      detail::uintmax_pair result
        = detail::file_size_api( ph.external_file_string() );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::file_size", ph, result.first ) );
      return result.second;
    }

    BOOST_FS_FUNC(space_info) space( const Path & ph )
    {
      detail::space_pair result
        = detail::space_api( ph.external_file_string() );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::space", ph, result.first ) );
      return result.second;
    }

    BOOST_FS_FUNC(std::time_t) last_write_time( const Path & ph )
    {
      detail::time_pair result
        = detail::last_write_time_api( ph.external_file_string() );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::last_write_time", ph, result.first ) );
      return result.second;
    }


//  operations  --------------------------------------------------------------//

    BOOST_FS_FUNC(bool) create_directory( const Path & dir_ph )
    {
      detail::query_pair result(
        detail::create_directory_api( dir_ph.external_directory_string() ) );
      if ( result.first != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::create_directory",
          dir_ph, result.first ) );
      return result.second;
    }

#if !defined(BOOST_WINDOWS_API) || defined(BOOST_FS_HARD_LINK)
    BOOST_FS_FUNC(void)
    create_hard_link( const Path & to_ph, const Path & from_ph )
    {
      system_error_type result( 
        detail::create_hard_link_api(
          to_ph.external_file_string(),
          from_ph.external_file_string() ) );
      if ( result != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::create_hard_link",
          to_ph, from_ph, result ) );
    }

    BOOST_FS_FUNC(system_error_type)
    create_hard_link( const Path & to_ph, const Path & from_ph,
      system_error_type & ec )
    {
      ec = detail::create_hard_link_api(
            to_ph.external_file_string(),
            from_ph.external_file_string() );
      return ec;
    }
#endif

    BOOST_FS_FUNC(void)
    create_symlink( const Path & to_ph, const Path & from_ph )
    {
      system_error_type result( 
        detail::create_symlink_api(
          to_ph.external_file_string(),
          from_ph.external_file_string() ) );
      if ( result )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::create_symlink",
          to_ph, from_ph, result ) );
    }

    BOOST_FS_FUNC(system_error_type)
    create_symlink( const Path & to_ph, const Path & from_ph,
      system_error_type & ec )
    {
      ec = detail::create_symlink_api(
             to_ph.external_file_string(),
             from_ph.external_file_string() );
      return ec;
    }

    BOOST_FS_FUNC(bool) remove( const Path & ph )
    {
      if ( exists( ph )
        || is_symlink( ph ) ) // handle dangling symbolic links
        // note that the POSIX behavior for symbolic links is what we want;
        // the link rather than what it points to is deleted. Windows behavior
        // doesn't matter; is_symlink() is always false on Windows.
      {
        system_error_type result = detail::remove_api( ph.external_file_string() );
        if ( result != 0 )
          boost::throw_exception( basic_filesystem_error<Path>(
            "boost::filesystem::remove",
            ph, result ) );
        return true;
      }
      return false;
    }

    BOOST_FS_FUNC(unsigned long) remove_all( const Path & ph )
    {
      return exists( ph )|| is_symlink( ph )
        ? detail::remove_all_aux( ph ) : 0;
    }

    BOOST_FS_FUNC(void) rename( const Path & from_path, const Path & to_path )
    {
      system_error_type result = detail::rename_api(
        from_path.external_directory_string(),
        to_path.external_directory_string() );
      if ( result != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::rename",
          from_path, to_path, result ) );
    }

    BOOST_FS_FUNC(void) copy_file( const Path & from_path, const Path & to_path )
    {
      system_error_type result = detail::copy_file_api(
        from_path.external_directory_string(),
        to_path.external_directory_string() );
      if ( result != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::copy_file",
          from_path, to_path, result ) );
    }

    template< class Path >
    Path current_path()
    {
      typename Path::external_string_type ph;
      boost::filesystem::system_error_type result;
      if ( (result = detail::get_current_path_api( ph )) != 0 )
          boost::throw_exception( basic_filesystem_error<Path>(
            "boost::filesystem::current_path", result ) );
      return Path( Path::traits_type::to_internal( ph ) );
    }

    template< class Path >
    const Path & initial_path()
    {
      static Path init_path;
      if ( init_path.empty() ) init_path = current_path<Path>();
      return init_path;
    }

# ifndef BOOST_FILESYSTEM_NO_DEPRECATED
    // legacy support
    inline path current_path()  // overload supports pre-i18n apps
      { return current_path<boost::filesystem::path>(); }
    inline const path & initial_path() // overload supports pre-i18n apps
      { return initial_path<boost::filesystem::path>(); }
# endif

    BOOST_FS_FUNC(Path) system_complete( const Path & ph )
    {
# ifdef BOOST_WINDOWS_API
      if ( ph.empty() ) return ph;
      BOOST_FS_TYPENAME Path::external_string_type sys_ph;
      boost::filesystem::system_error_type result;
      if ( (result = detail::get_full_path_name_api( ph.external_file_string(),
              sys_ph )) != 0 )
          boost::throw_exception( basic_filesystem_error<Path>(
            "boost::filesystem::system_complete", ph, result ) );
      return Path( Path::traits_type::to_internal( sys_ph ) );
# else
      return (ph.empty() || ph.is_complete())
        ? ph : current_path<Path>() / ph;
# endif
    }

    BOOST_FS_FUNC(Path)
    complete( const Path & ph,
      const Path & base/* = initial_path<Path>() */)
    {
      BOOST_ASSERT( base.is_complete()
        && (ph.is_complete() || !ph.has_root_name())
        && "boost::filesystem::complete() precondition not met" );
#   ifdef BOOST_WINDOWS_PATH
      if (ph.empty() || ph.is_complete()) return ph;
      if ( !ph.has_root_name() )
        return ph.has_root_directory()
          ? Path( base.root_name() ) / ph
          : base / ph;
      return base / ph;
#   else
      return (ph.empty() || ph.is_complete()) ? ph : base / ph;
#   endif
    }

    // VC++ 7.1 had trouble with default arguments, so separate one argument
    // signatures are provided as workarounds; the effect is the same.
    BOOST_FS_FUNC(Path) complete( const Path & ph )
      { return complete( ph, initial_path<Path>() ); }

    BOOST_FS_FUNC(void)
    last_write_time( const Path & ph, const std::time_t new_time )
    {
      boost::filesystem::system_error_type result;
      if ( (result = detail::last_write_time_api( ph.external_file_string(),
          new_time )) != 0 )
        boost::throw_exception( basic_filesystem_error<Path>(
          "boost::filesystem::last_write_time", ph, result ) );
    }

# ifndef BOOST_FILESYSTEM_NARROW_ONLY

    // "do-the-right-thing" overloads  ---------------------------------------//

    inline file_status status( const path & ph )
      { return status<path>( ph ); }
    inline file_status status( const wpath & ph )
      { return status<wpath>( ph ); }

    inline file_status status( const path & ph, system_error_type & ec )
      { return status<path>( ph, ec ); }
    inline file_status status( const wpath & ph, system_error_type & ec )
      { return status<wpath>( ph, ec ); }

    inline file_status symlink_status( const path & ph )
      { return symlink_status<path>( ph ); }
    inline file_status symlink_status( const wpath & ph )
      { return symlink_status<wpath>( ph ); }

    inline file_status symlink_status( const path & ph, system_error_type & ec )
      { return symlink_status<path>( ph, ec ); }
    inline file_status symlink_status( const wpath & ph, system_error_type & ec )
      { return symlink_status<wpath>( ph, ec ); }

    inline bool exists( const path & ph ) { return exists<path>( ph ); }
    inline bool exists( const wpath & ph ) { return exists<wpath>( ph ); }

    inline bool is_directory( const path & ph )
      { return is_directory<path>( ph ); }
    inline bool is_directory( const wpath & ph )
      { return is_directory<wpath>( ph ); }
 
    inline bool is_regular( const path & ph )
      { return is_regular<path>( ph ); }
    inline bool is_regular( const wpath & ph )
      { return is_regular<wpath>( ph ); }

    inline bool is_other( const path & ph )
      { return is_other<path>( ph ); }
    inline bool is_other( const wpath & ph )
      { return is_other<wpath>( ph ); }

    inline bool is_symlink( const path & ph )
      { return is_symlink<path>( ph ); }
    inline bool is_symlink( const wpath & ph )
      { return is_symlink<wpath>( ph ); }

    inline bool is_empty( const path & ph )
      { return is_empty<path>( ph ); }
    inline bool is_empty( const wpath & ph )
      { return is_empty<wpath>( ph ); }

    inline bool equivalent( const path & ph1, const path & ph2 )
      { return equivalent<path>( ph1, ph2 ); }
    inline bool equivalent( const wpath & ph1, const wpath & ph2 )
      { return equivalent<wpath>( ph1, ph2 ); }

    inline boost::uintmax_t file_size( const path & ph )
      { return file_size<path>( ph ); }
    inline boost::uintmax_t file_size( const wpath & ph )
      { return file_size<wpath>( ph ); }

    inline space_info space( const path & ph )
      { return space<path>( ph ); }
    inline space_info space( const wpath & ph )
      { return space<wpath>( ph ); }

    inline std::time_t last_write_time( const path & ph )
      { return last_write_time<path>( ph ); }
    inline std::time_t last_write_time( const wpath & ph )
      { return last_write_time<wpath>( ph ); }

    inline bool create_directory( const path & dir_ph )
      { return create_directory<path>( dir_ph ); }
    inline bool create_directory( const wpath & dir_ph )
      { return create_directory<wpath>( dir_ph ); }

#if !defined(BOOST_WINDOWS_API) || defined(BOOST_FS_HARD_LINK)
    inline void create_hard_link( const path & to_ph,
      const path & from_ph )
      { return create_hard_link<path>( to_ph, from_ph ); }
    inline void create_hard_link( const wpath & to_ph,
      const wpath & from_ph )
      { return create_hard_link<wpath>( to_ph, from_ph ); }

    inline system_error_type create_hard_link( const path & to_ph,
      const path & from_ph, system_error_type & ec )
      { return create_hard_link<path>( to_ph, from_ph, ec ); }
    inline system_error_type create_hard_link( const wpath & to_ph,
      const wpath & from_ph, system_error_type & ec )
      { return create_hard_link<wpath>( to_ph, from_ph, ec ); }
#endif
    
    inline void create_symlink( const path & to_ph,
      const path & from_ph )
      { return create_symlink<path>( to_ph, from_ph ); }
    inline void create_symlink( const wpath & to_ph,
      const wpath & from_ph )
      { return create_symlink<wpath>( to_ph, from_ph ); }

    inline system_error_type create_symlink( const path & to_ph,
      const path & from_ph, system_error_type & ec )
      { return create_symlink<path>( to_ph, from_ph, ec ); }
    inline system_error_type create_symlink( const wpath & to_ph,
      const wpath & from_ph, system_error_type & ec )
      { return create_symlink<wpath>( to_ph, from_ph, ec ); }

    inline bool remove( const path & ph )
      { return remove<path>( ph ); }
    inline bool remove( const wpath & ph )
      { return remove<wpath>( ph ); }

    inline unsigned long remove_all( const path & ph )
      { return remove_all<path>( ph ); }
    inline unsigned long remove_all( const wpath & ph )
      { return remove_all<wpath>( ph ); }

    inline void rename( const path & from_path, const path & to_path )
      { return rename<path>( from_path, to_path ); }
    inline void rename( const wpath & from_path, const wpath & to_path )
      { return rename<wpath>( from_path, to_path ); }

    inline void copy_file( const path & from_path, const path & to_path )
      { return copy_file<path>( from_path, to_path ); }
    inline void copy_file( const wpath & from_path, const wpath & to_path )
      { return copy_file<wpath>( from_path, to_path ); }

    inline path system_complete( const path & ph )
      { return system_complete<path>( ph ); }
    inline wpath system_complete( const wpath & ph )
      { return system_complete<wpath>( ph ); }

    inline path complete( const path & ph,
      const path & base/* = initial_path<path>()*/ )
      { return complete<path>( ph, base ); }
    inline wpath complete( const wpath & ph,
      const wpath & base/* = initial_path<wpath>()*/ )
      { return complete<wpath>( ph, base ); }

    inline path complete( const path & ph )
      { return complete<path>( ph, initial_path<path>() ); }
    inline wpath complete( const wpath & ph )
      { return complete<wpath>( ph, initial_path<wpath>() ); }

    inline void last_write_time( const path & ph, const std::time_t new_time )
      { last_write_time<path>( ph, new_time ); }
    inline void last_write_time( const wpath & ph, const std::time_t new_time )
      { last_write_time<wpath>( ph, new_time ); }

# endif // BOOST_FILESYSTEM_NARROW_ONLY

    namespace detail
    {
      template<class Path>
      unsigned long remove_all_aux( const Path & ph )
      {
        static const boost::filesystem::basic_directory_iterator<Path> end_itr;
        unsigned long count = 1;
        if ( !boost::filesystem::is_symlink( ph ) // don't recurse symbolic links
          && boost::filesystem::is_directory( ph ) )
        {
          for ( boost::filesystem::basic_directory_iterator<Path> itr( ph );
                itr != end_itr; ++itr )
          {
            count += remove_all_aux( itr->path() );
          }
        }
        boost::filesystem::remove( ph );
        return count;
      }

//  test helper  -------------------------------------------------------------//

    // not part of the documented interface because false positives are possible;
    // there is no law that says that an OS that has large stat.st_size
    // actually supports large file sizes.
      BOOST_FILESYSTEM_DECL bool possible_large_file_size_support();

//  directory_iterator helpers  ----------------------------------------------//

//    forwarding functions avoid need for BOOST_FILESYSTEM_DECL for class
//    basic_directory_iterator, and so avoid iterator_facade DLL template
//    problems. They also overload to the proper external path character type.

      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        dir_itr_first( void *& handle,
#if       defined(BOOST_POSIX_API)
            void *& buffer,
#endif
          const std::string & dir_path,
          std::string & target, file_status & fs, file_status & symlink_fs );
      // eof: return==0 && handle==0

      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        dir_itr_increment( void *& handle,
#if       defined(BOOST_POSIX_API)
            void *& buffer,
#endif
          std::string & target, file_status & fs, file_status & symlink_fs );
      // eof: return==0 && handle==0

      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        dir_itr_close( void *& handle
#if       defined(BOOST_POSIX_API)
            , void *& buffer
#endif
          );
      // Effects: none if handle==0, otherwise close handle, set handle=0

#     if defined(BOOST_WINDOWS_API) && !defined(BOOST_FILESYSTEM_NARROW_ONLY)
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        dir_itr_first( void *& handle, const std::wstring & ph,
          std::wstring & target, file_status & fs, file_status & symlink_fs );
      BOOST_FILESYSTEM_DECL boost::filesystem::system_error_type
        dir_itr_increment( void *& handle, std::wstring & target,
          file_status & fs, file_status & symlink_fs );
#     endif

      template< class Path >
      class dir_itr_imp
      {
      public:  
        basic_directory_entry<Path> m_directory_entry;
        void *        m_handle;
#       ifdef BOOST_POSIX_API
          void *      m_buffer;  // see dir_itr_increment implementation
#       endif
        dir_itr_imp() : m_handle(0)
#       ifdef BOOST_POSIX_API
          , m_buffer(0)
#       endif
        {}

        ~dir_itr_imp() { dir_itr_close( m_handle
#if       defined(BOOST_POSIX_API)
            , m_buffer
#endif
          ); }
      };

    BOOST_FILESYSTEM_DECL extern system_error_type not_found_error;
    } // namespace detail

//  basic_directory_iterator  ------------------------------------------------//

    template< class Path >
    class basic_directory_iterator
      : public boost::iterator_facade<
          basic_directory_iterator<Path>,
          basic_directory_entry<Path>,
          boost::single_pass_traversal_tag >
    {
    public:
      typedef Path path_type;

      basic_directory_iterator(){}  // creates the "end" iterator

      explicit basic_directory_iterator( const Path & dir_path );
      basic_directory_iterator( const Path & dir_path, system_error_type & ec );

    private:

      // shared_ptr provides shallow-copy semantics required for InputIterators.
      // m_imp.get()==0 indicates the end iterator.
      boost::shared_ptr< detail::dir_itr_imp< Path > >  m_imp;

      friend class boost::iterator_core_access;

      typename boost::iterator_facade<
        basic_directory_iterator<Path>,
        basic_directory_entry<Path>,
        boost::single_pass_traversal_tag >::reference dereference() const 
      {
        BOOST_ASSERT( m_imp.get() && "attempt to dereference end iterator" );
        return m_imp->m_directory_entry;
      }

      void increment();

      bool equal( const basic_directory_iterator & rhs ) const
        { return m_imp == rhs.m_imp; }

      system_error_type m_init( const Path & dir_path );
    };

    typedef basic_directory_iterator< path > directory_iterator;
# ifndef BOOST_FILESYSTEM_NARROW_ONLY
    typedef basic_directory_iterator< wpath > wdirectory_iterator;
# endif

    //  basic_directory_iterator implementation  ---------------------------//

    template<class Path>
    system_error_type basic_directory_iterator<Path>::m_init(
      const Path & dir_path )
    {
      if ( dir_path.empty() )
      {
        m_imp.reset();
        return detail::not_found_error;
      }
      system_error_type sys_err;
      typename Path::external_string_type name;
      file_status fs, symlink_fs;

      if ( (sys_err = detail::dir_itr_first( m_imp->m_handle,
#if   defined(BOOST_POSIX_API)
        m_imp->m_buffer,
#endif
        dir_path.external_directory_string(),
        name, fs, symlink_fs )) != 0 )
      {
        m_imp.reset();
        return sys_err;
      }
      
      if ( m_imp->m_handle == 0 ) m_imp.reset(); // eof, so make end iterator
      else // not eof
      {
        m_imp->m_directory_entry.assign( dir_path
          / Path::traits_type::to_internal( name ), fs, symlink_fs );
        if ( name[0] == dot<Path>::value // dot or dot-dot
          && (name.size() == 1
            || (name[1] == dot<Path>::value
              && name.size() == 2)) )
          {  increment(); }
      }
      return 0;
    }

    template<class Path>
    basic_directory_iterator<Path>::basic_directory_iterator(
      const Path & dir_path )
      : m_imp( new detail::dir_itr_imp<Path> )
    {
      system_error_type ec( m_init(dir_path) );
      if ( ec != 0 )
      {
        boost::throw_exception( basic_filesystem_error<Path>(  
          "boost::filesystem::basic_directory_iterator constructor",
          dir_path, ec ) );
      }
    }

    template<class Path>
    basic_directory_iterator<Path>::basic_directory_iterator(
      const Path & dir_path, system_error_type & ec )
      : m_imp( new detail::dir_itr_imp<Path> )
    {
      ec = m_init(dir_path);
    }

    template<class Path>
    void basic_directory_iterator<Path>::increment()
    {
      BOOST_ASSERT( m_imp.get() && "attempt to increment end iterator" );
      BOOST_ASSERT( m_imp->m_handle != 0 && "internal program error" );
      
      system_error_type sys_err(0);
      typename Path::external_string_type name;
      file_status fs, symlink_fs;

      for (;;)
      {
        if ( (sys_err = detail::dir_itr_increment( m_imp->m_handle,
#if     defined(BOOST_POSIX_API)
          m_imp->m_buffer,
#endif
          name, fs, symlink_fs )) != 0 )
        {
          boost::throw_exception( basic_filesystem_error<Path>(  
            "boost::filesystem::basic_directory_iterator increment",
            m_imp->m_directory_entry.path().branch_path(), sys_err ) );
        }
        if ( m_imp->m_handle == 0 ) { m_imp.reset(); return; } // eof, make end
        if ( !(name[0] == dot<Path>::value // !(dot or dot-dot)
          && (name.size() == 1
            || (name[1] == dot<Path>::value
              && name.size() == 2))) )
        {
          m_imp->m_directory_entry.replace_leaf(
            Path::traits_type::to_internal( name ), fs, symlink_fs );
          return;
        }
      }
    }

    //  basic_directory_entry  -----------------------------------------------//
    
    template<class Path>
    class basic_directory_entry
    {
    public:
      typedef Path path_type;
      typedef typename Path::string_type string_type;

      // compiler generated copy-ctor, copy assignment, and destructor apply

      basic_directory_entry() {}
      explicit basic_directory_entry( const path_type & p,
        file_status st = file_status(), file_status symlink_st=file_status() )
        : m_path(p), m_status(st), m_symlink_status(symlink_st)
        {}

      void assign( const path_type & p,
        file_status st, file_status symlink_st )
        { m_path = p; m_status = st; m_symlink_status = symlink_st; }

      void replace_leaf( const string_type & s,
        file_status st, file_status symlink_st )
     {
       m_path.remove_leaf();
       m_path /= s;
       m_status = st;
       m_symlink_status = symlink_st;
     }

      const Path &   path() const { return m_path; }
      file_status   status() const;
      file_status   status( system_error_type & ec ) const;
      file_status   symlink_status() const;
      file_status   symlink_status( system_error_type & ec ) const;

      // conversion simplifies the most common use of basic_directory_entry
      operator const path_type &() const { return m_path; }

#   ifndef BOOST_FILESYSTEM_NO_DEPRECATED
      // deprecated functions preserve common use cases in legacy code
      typename Path::string_type leaf() const
      {
        return path().leaf();
      }
      typename Path::string_type string() const
      {
        return path().string();
      }
#   endif

    private:
      path_type             m_path;
      mutable file_status  m_status;           // stat()-like
      mutable file_status  m_symlink_status;   // lstat()-like
        // note: m_symlink_status is not used by Windows implementation

    }; // basic_directory_status

    typedef basic_directory_entry<path> directory_entry;
# ifndef BOOST_FILESYSTEM_NARROW_ONLY
    typedef basic_directory_entry<wpath> wdirectory_entry;
# endif

    //  basic_directory_entry implementation  --------------------------------//

    template<class Path>
    file_status
    basic_directory_entry<Path>::status() const
    {
      if ( !status_known( m_status ) )
      {
#     ifndef BOOST_WINDOWS_API
        if ( status_known( m_symlink_status )
          && !is_symlink( m_symlink_status ) )
          { m_status = m_symlink_status; }
        else { m_status = boost::filesystem::status( m_path ); }
#     else
        m_status = boost::filesystem::status( m_path );
#     endif
      }
      return m_status;
    }

    template<class Path>
    file_status
    basic_directory_entry<Path>::status( system_error_type & ec ) const
    {
      if ( !status_known( m_status ) )
      {
#     ifndef BOOST_WINDOWS_API
        if ( status_known( m_symlink_status )
          && !is_symlink( m_symlink_status ) )
          { ec = 0; m_status = m_symlink_status; }
        else { m_status = boost::filesystem::status( m_path, ec ); }
#     else
        m_status = boost::filesystem::status( m_path, ec );
#     endif
      }
      else ec = 0;
      return m_status;
    }

    template<class Path>
    file_status
    basic_directory_entry<Path>::symlink_status() const
    {
#   ifndef BOOST_WINDOWS_API
      if ( !status_known( m_symlink_status ) )
        { m_symlink_status = boost::filesystem::symlink_status( m_path ); }
      return m_symlink_status;
#   else
      return status();
#   endif
    }

    template<class Path>
    file_status
    basic_directory_entry<Path>::symlink_status( system_error_type & ec ) const
    {
#   ifndef BOOST_WINDOWS_API
      if ( !status_known( m_symlink_status ) )
        { m_symlink_status = boost::filesystem::symlink_status( m_path, ec ); }
      else ec = 0;
      return m_symlink_status;
#   else
      return status( ec );
#   endif
    }
  } // namespace filesystem
} // namespace boost

#undef BOOST_FS_FUNC


#include <boost/config/abi_suffix.hpp> // pops abi_prefix.hpp pragmas
#endif // BOOST_FILESYSTEM_OPERATIONS_HPP
