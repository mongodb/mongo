//  boost/filesystem/path.hpp  -----------------------------------------------//

//  Copyright Beman Dawes 2002-2005
//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

//----------------------------------------------------------------------------// 

#ifndef BOOST_FILESYSTEM_PATH_HPP
#define BOOST_FILESYSTEM_PATH_HPP

#include <boost/filesystem/config.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/throw_exception.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/static_assert.hpp>

#include <string>
#include <algorithm> // for lexicographical_compare
#include <iosfwd>    // needed by basic_path inserter and extractor
#include <stdexcept>
#include <cassert>

# ifndef BOOST_FILESYSTEM_NARROW_ONLY
#   include <locale>
# endif

#include <boost/config/abi_prefix.hpp> // must be the last #include

//----------------------------------------------------------------------------//

namespace boost
{
  namespace BOOST_FILESYSTEM_NAMESPACE
  {
    template<class String, class Traits> class basic_path;

    struct path_traits;
    typedef basic_path< std::string, path_traits > path;

    struct path_traits
    {
      typedef std::string internal_string_type;
      typedef std::string external_string_type;
      static external_string_type to_external( const path &,
        const internal_string_type & src ) { return src; }
      static internal_string_type to_internal(
        const external_string_type & src ) { return src; }
    };

# ifndef BOOST_FILESYSTEM_NARROW_ONLY

    struct wpath_traits;
    
    typedef basic_path< std::wstring, wpath_traits > wpath;

    struct wpath_traits
    {
      typedef std::wstring internal_string_type;
# ifdef BOOST_WINDOWS_API
      typedef std::wstring external_string_type;
      static external_string_type to_external( const wpath &,
        const internal_string_type & src ) { return src; }
      static internal_string_type to_internal(
        const external_string_type & src ) { return src; }
# else
      typedef std::string external_string_type;
      static external_string_type to_external( const wpath & ph,
        const internal_string_type & src );
      static internal_string_type to_internal(
        const external_string_type & src );
# endif
      static void imbue( const std::locale & loc );
      static bool imbue( const std::locale & loc, const std::nothrow_t & );
    };

# endif // ifndef BOOST_FILESYSTEM_NARROW_ONLY

//  error reporting support  -------------------------------------------------//

    typedef int errno_type;  // determined by C standard
    
# ifdef BOOST_WINDOWS_API
    typedef unsigned system_error_type;

    BOOST_FILESYSTEM_DECL
    errno_type lookup_errno( system_error_type sys_err_code );
# else
    typedef int system_error_type;

    inline errno_type lookup_errno( system_error_type sys_err_code )
      { return sys_err_code; }
# endif

    // deprecated support for legacy function name
    inline errno_type lookup_error_code( system_error_type sys_err_code )
      { return lookup_errno( sys_err_code ); }

    BOOST_FILESYSTEM_DECL
    void system_message( system_error_type sys_err_code, std::string & target );
    // Effects: appends error message to target

# if defined(BOOST_WINDOWS_API) && !defined(BOOST_FILESYSTEM_NARROW_ONLY)
    BOOST_FILESYSTEM_DECL void
    system_message( system_error_type sys_err_code, std::wstring & target );
# endif

    //  filesystem_error  ----------------------------------------------------//

    class filesystem_error : public std::runtime_error
    // see http://www.boost.org/more/error_handling.html for design rationale
    {
    public:
      filesystem_error()
        : std::runtime_error("filesystem error"), m_sys_err(0) {}
      explicit filesystem_error(
        const std::string & what_arg, system_error_type sys_ec = 0 )
        : std::runtime_error(what_arg), m_sys_err(sys_ec) {}

      system_error_type  system_error() const { return m_sys_err; }
      // Note: system_error() == 0 implies a library (rather than system) error

    private:
      system_error_type m_sys_err;
    };

    //  basic_filesystem_error  ----------------------------------------------//

    template<class Path>
    class basic_filesystem_error : public filesystem_error
    {
    // see http://www.boost.org/more/error_handling.html for design rationale
    public:
      // compiler generates copy constructor and copy assignment

      typedef Path path_type;

      basic_filesystem_error( const std::string & what,
        system_error_type sys_err_code );

      basic_filesystem_error( const std::string & what,
        const path_type & path1, system_error_type sys_err_code );

      basic_filesystem_error( const std::string & what, const path_type & path1,
        const path_type & path2, system_error_type sys_err_code );

      ~basic_filesystem_error() throw() {}

      const path_type & path1() const
      {
        static const path_type empty_path;
        return m_imp_ptr.get() ? m_imp_ptr->m_path1 : empty_path ;
      }
      const path_type & path2() const
      {
        static const path_type empty_path;
        return m_imp_ptr.get() ? m_imp_ptr->m_path2 : empty_path ;
      }

    private:
      struct m_imp
      {
        path_type       m_path1; // may be empty()
        path_type       m_path2; // may be empty()
      };
      boost::shared_ptr<m_imp> m_imp_ptr;
    };

    typedef basic_filesystem_error<path> filesystem_path_error;

# ifndef BOOST_FILESYSTEM_NARROW_ONLY
    typedef basic_filesystem_error<wpath> filesystem_wpath_error;
# endif

    //  path traits  ---------------------------------------------------------//

    template<class Path> struct is_basic_path
      { BOOST_STATIC_CONSTANT( bool, value = false ); };
    template<> struct is_basic_path<path>
      { BOOST_STATIC_CONSTANT( bool, value = true ); };
# ifndef BOOST_FILESYSTEM_NARROW_ONLY
    template<> struct is_basic_path<wpath>
      { BOOST_STATIC_CONSTANT( bool, value = true ); };
# endif

    // these only have to be specialized if Path::string_type::value_type
    // is not convertible from char
    template<class Path> struct slash
      { BOOST_STATIC_CONSTANT( char, value = '/' ); };

    template<class Path> struct dot
      { BOOST_STATIC_CONSTANT( char, value = '.' ); };

    template<class Path> struct colon
      { BOOST_STATIC_CONSTANT( char, value = ':' ); };

# ifdef BOOST_WINDOWS_PATH
    template<class Path> struct path_alt_separator
      { BOOST_STATIC_CONSTANT( char, value = '\\' ); };
# endif

    //  workaround for VC++ 7.0 and earlier issues with nested classes
    namespace detail
    {
      template<class Path>
      class iterator_helper
      {
      public:
        typedef typename Path::iterator iterator;
        static void do_increment( iterator & ph );
        static void do_decrement( iterator & ph );
      };
    }

    //  basic_path  ----------------------------------------------------------//
  
    template<class String, class Traits>
    class basic_path
    {
    // invariant: m_path valid according to the portable generic path grammar

      // validate template arguments
// TODO: get these working
//      BOOST_STATIC_ASSERT( ::boost::is_same<String,typename Traits::internal_string_type>::value );
//      BOOST_STATIC_ASSERT( ::boost::is_same<typename Traits::external_string_type,std::string>::value || ::boost::is_same<typename Traits::external_string_type,std::wstring>::value );

    public:
      // compiler generates copy constructor and copy assignment

      typedef basic_path<String, Traits> path_type;
      typedef String string_type;
      typedef typename String::value_type value_type;
      typedef Traits traits_type;
      typedef typename Traits::external_string_type external_string_type; 

      // constructors/destructor
      basic_path() {}
      basic_path( const string_type & s ) { operator/=( s ); }
      basic_path( const value_type * s )  { operator/=( s ); }
#     ifndef BOOST_NO_MEMBER_TEMPLATES
        template <class InputIterator>
          basic_path( InputIterator first, InputIterator last )
            { append( first, last ); }
#     endif
     ~basic_path() {}

      // assignments
      basic_path & operator=( const string_type & s )
      {
#     if BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, >= 310)
        m_path.clear();
#     else
        m_path.erase( m_path.begin(), m_path.end() );
#     endif
        operator/=( s ); 
        return *this;
      }
      basic_path & operator=( const value_type * s )
      { 
#     if BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, >= 310)
        m_path.clear();
#     else
        m_path.erase( m_path.begin(), m_path.end() );
#     endif
        operator/=( s ); 
        return *this;
      }
#     ifndef BOOST_NO_MEMBER_TEMPLATES
        template <class InputIterator>
          basic_path & assign( InputIterator first, InputIterator last )
            { m_path.clear(); append( first, last ); return *this; }
#     endif

      // modifiers
      basic_path & operator/=( const basic_path & rhs )  { return operator /=( rhs.string().c_str() ); }
      basic_path & operator/=( const string_type & rhs ) { return operator /=( rhs.c_str() ); }
      basic_path & operator/=( const value_type * s );
#     ifndef BOOST_NO_MEMBER_TEMPLATES
        template <class InputIterator>
          basic_path & append( InputIterator first, InputIterator last );
#     endif
      
      void swap( basic_path & rhs )
      {
        m_path.swap( rhs.m_path );
#       ifdef BOOST_CYGWIN_PATH
          std::swap( m_cygwin_root, rhs.m_cygwin_root );
#       endif
      }

      basic_path & remove_leaf();

      // observers
      const string_type & string() const         { return m_path; }
      const string_type file_string() const;
      const string_type directory_string() const { return file_string(); }

      const external_string_type external_file_string() const { return Traits::to_external( *this, file_string() ); }
      const external_string_type external_directory_string() const { return Traits::to_external( *this, directory_string() ); }

      basic_path   root_path() const;
      string_type  root_name() const;
      string_type  root_directory() const;
      basic_path   relative_path() const;
      string_type  leaf() const;
      basic_path   branch_path() const;

      bool empty() const               { return m_path.empty(); } // name consistent with std containers
      bool is_complete() const;
      bool has_root_path() const;
      bool has_root_name() const;
      bool has_root_directory() const;
      bool has_relative_path() const   { return !relative_path().empty(); }
      bool has_leaf() const            { return !m_path.empty(); }
      bool has_branch_path() const     { return !branch_path().empty(); }

      // iterators
      class iterator : public boost::iterator_facade<
        iterator,
        string_type const,
        boost::bidirectional_traversal_tag >
      {
      private:
        friend class boost::iterator_core_access;
        friend class boost::BOOST_FILESYSTEM_NAMESPACE::basic_path<String, Traits>;

        const string_type & dereference() const
          { return m_name; }
        bool equal( const iterator & rhs ) const
          { return m_path_ptr == rhs.m_path_ptr && m_pos == rhs.m_pos; }

        friend class boost::BOOST_FILESYSTEM_NAMESPACE::detail::iterator_helper<path_type>;

        void increment()
        { 
          boost::BOOST_FILESYSTEM_NAMESPACE::detail::iterator_helper<path_type>::do_increment(
            *this );
        }
        void decrement()
        { 
          boost::BOOST_FILESYSTEM_NAMESPACE::detail::iterator_helper<path_type>::do_decrement(
            *this );
        }

        string_type             m_name;     // current element
        const basic_path *      m_path_ptr; // path being iterated over
        typename string_type::size_type  m_pos;  // position of name in
                                            // path_ptr->string(). The
                                            // end() iterator is indicated by 
                                            // pos == path_ptr->m_path.size()
      }; // iterator

      typedef iterator const_iterator;

      iterator begin() const;
      iterator end() const;

    private:
      // Note: This is an implementation for POSIX and Windows, where there
      // are only minor differences between generic and native path grammars.
      // Private members might be quite different in other implementations,
      // particularly where there were wide differences between portable and
      // native path formats, or between file_string() and
      // directory_string() formats, or simply that the implementation
      // was willing expend additional memory to achieve greater speed for
      // some operations at the expense of other operations.

      string_type  m_path; // invariant: portable path grammar
                           // on Windows, backslashes converted to slashes

#   ifdef BOOST_CYGWIN_PATH
      bool m_cygwin_root; // if present, m_path[0] was slash. note: initialization
                          // done by append
#   endif  

      void m_append_separator_if_needed();
      void m_append( value_type value ); // converts Windows alt_separator

      // Was qualified; como433beta8 reports:
      //    warning #427-D: qualified name is not allowed in member declaration 
      friend class iterator;
      friend class boost::BOOST_FILESYSTEM_NAMESPACE::detail::iterator_helper<path_type>;

      // Deprecated features ease transition for existing code. Don't use these
      // in new code.
# ifndef BOOST_FILESYSTEM_NO_DEPRECATED
    public:
      typedef bool (*name_check)( const std::string & name );
      basic_path( const string_type & str, name_check ) { operator/=( str ); }
      basic_path( const typename string_type::value_type * s, name_check )
        { operator/=( s );}
      string_type native_file_string() const { return file_string(); }
      string_type native_directory_string() const { return directory_string(); }
      static bool default_name_check_writable() { return false; } 
      static void default_name_check( name_check ) {}
      static name_check default_name_check() { return 0; }
      basic_path & canonize();
      basic_path & normalize();
# endif
    };

  //  basic_path non-member functions  ---------------------------------------//

    template< class String, class Traits >
    inline void swap( basic_path<String, Traits> & lhs,
               basic_path<String, Traits> & rhs ) { lhs.swap( rhs ); }

    template< class String, class Traits >
    bool operator<( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs )
    {
      return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end() );
    }

    template< class String, class Traits >
    bool operator<( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs )
    {
      basic_path<String, Traits> tmp( lhs );
      return std::lexicographical_compare(
        tmp.begin(), tmp.end(), rhs.begin(), rhs.end() );
    }

    template< class String, class Traits >
    bool operator<( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs )
    {
      basic_path<String, Traits> tmp( lhs );
      return std::lexicographical_compare(
        tmp.begin(), tmp.end(), rhs.begin(), rhs.end() );
    }

    template< class String, class Traits >
    bool operator<( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
    {
      basic_path<String, Traits> tmp( rhs );
      return std::lexicographical_compare(
        lhs.begin(), lhs.end(), tmp.begin(), tmp.end() );
    }

    template< class String, class Traits >
    bool operator<( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
    {
      basic_path<String, Traits> tmp( rhs );
      return std::lexicographical_compare(
        lhs.begin(), lhs.end(), tmp.begin(), tmp.end() );
    }

    template< class String, class Traits >
    inline bool operator==( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs )
    { 
      return !(lhs < rhs) && !(rhs < lhs);
    }

    template< class String, class Traits >
    inline bool operator==( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs )
    {
      basic_path<String, Traits> tmp( lhs );
      return !(tmp < rhs) && !(rhs < tmp);
    }

    template< class String, class Traits >
    inline bool operator==( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs )
    {
      basic_path<String, Traits> tmp( lhs );
      return !(tmp < rhs) && !(rhs < tmp);
    }

    template< class String, class Traits >
    inline bool operator==( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
    {
      basic_path<String, Traits> tmp( rhs );
      return !(lhs < tmp) && !(tmp < lhs);
    }

    template< class String, class Traits >
    inline bool operator==( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
    {
      basic_path<String, Traits> tmp( rhs );
      return !(lhs < tmp) && !(tmp < lhs);
    }

    template< class String, class Traits >
    inline bool operator!=( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs ) { return !(lhs == rhs); }
    
    template< class String, class Traits >
    inline bool operator!=( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs ) { return !(basic_path<String, Traits>(lhs) == rhs); }

    template< class String, class Traits >
    inline bool operator!=( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs ) { return !(basic_path<String, Traits>(lhs) == rhs); }

    template< class String, class Traits >
    inline bool operator!=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
                    { return !(lhs == basic_path<String, Traits>(rhs)); }

    template< class String, class Traits >
    inline bool operator!=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
                    { return !(lhs == basic_path<String, Traits>(rhs)); }

    template< class String, class Traits >
    inline bool operator>( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs ) { return rhs < lhs; }
    
    template< class String, class Traits >
    inline bool operator>( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs ) { return rhs < basic_path<String, Traits>(lhs); }

    template< class String, class Traits >
    inline bool operator>( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs ) { return rhs < basic_path<String, Traits>(lhs); }

    template< class String, class Traits >
    inline bool operator>( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
                    { return basic_path<String, Traits>(rhs) < lhs; }

    template< class String, class Traits >
    inline bool operator>( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
                    { return basic_path<String, Traits>(rhs) < lhs; }

    template< class String, class Traits >
    inline bool operator<=( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs ) { return !(rhs < lhs); }
    
    template< class String, class Traits >
    inline bool operator<=( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs ) { return !(rhs < basic_path<String, Traits>(lhs)); }

    template< class String, class Traits >
    inline bool operator<=( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs ) { return !(rhs < basic_path<String, Traits>(lhs)); }

    template< class String, class Traits >
    inline bool operator<=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
                    { return !(basic_path<String, Traits>(rhs) < lhs); }

    template< class String, class Traits >
    inline bool operator<=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
                    { return !(basic_path<String, Traits>(rhs) < lhs); }

    template< class String, class Traits >
    inline bool operator>=( const basic_path<String, Traits> & lhs, const basic_path<String, Traits> & rhs ) { return !(lhs < rhs); }
    
    template< class String, class Traits >
    inline bool operator>=( const typename basic_path<String, Traits>::string_type::value_type * lhs,
                    const basic_path<String, Traits> & rhs ) { return !(lhs < basic_path<String, Traits>(rhs)); }

    template< class String, class Traits >
    inline bool operator>=( const typename basic_path<String, Traits>::string_type & lhs,
                    const basic_path<String, Traits> & rhs ) { return !(lhs < basic_path<String, Traits>(rhs)); }

    template< class String, class Traits >
    inline bool operator>=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type::value_type * rhs )
                    { return !(basic_path<String, Traits>(lhs) < rhs); }

    template< class String, class Traits >
    inline bool operator>=( const basic_path<String, Traits> & lhs,
                    const typename basic_path<String, Traits>::string_type & rhs )
                    { return !(basic_path<String, Traits>(lhs) < rhs); }

    // operator /

    template< class String, class Traits >
    inline basic_path<String, Traits> operator/( 
      const basic_path<String, Traits> & lhs,
      const basic_path<String, Traits> & rhs )
      { return basic_path<String, Traits>( lhs ) /= rhs; }

    template< class String, class Traits >
    inline basic_path<String, Traits> operator/( 
      const basic_path<String, Traits> & lhs,
      const typename String::value_type * rhs )
      { return basic_path<String, Traits>( lhs ) /=
          basic_path<String, Traits>( rhs ); }

    template< class String, class Traits >
    inline basic_path<String, Traits> operator/( 
      const basic_path<String, Traits> & lhs, const String & rhs )
      { return basic_path<String, Traits>( lhs ) /=
          basic_path<String, Traits>( rhs ); }

    template< class String, class Traits >
    inline basic_path<String, Traits> operator/( 
      const typename String::value_type * lhs,
      const basic_path<String, Traits> & rhs )
      { return basic_path<String, Traits>( lhs ) /= rhs; }

    template< class String, class Traits >
    inline basic_path<String, Traits> operator/(
      const String & lhs, const basic_path<String, Traits> & rhs )
      { return basic_path<String, Traits>( lhs ) /= rhs; }
   
    //  inserters and extractors  --------------------------------------------//

// bypass VC++ 7.0 and earlier, and broken Borland compilers
# if !BOOST_WORKAROUND(BOOST_MSVC, <= 1300) && !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
    template< class Path >
    std::basic_ostream< typename Path::string_type::value_type,
      typename Path::string_type::traits_type > &
      operator<<
      ( std::basic_ostream< typename Path::string_type::value_type,
      typename Path::string_type::traits_type >& os, const Path & ph )
    {
      os << ph.string();
      return os;
    }

    template< class Path >
    std::basic_istream< typename Path::string_type::value_type,
      typename Path::string_type::traits_type > &
      operator>>
      ( std::basic_istream< typename Path::string_type::value_type,
      typename Path::string_type::traits_type >& is, Path & ph )
    {
      typename Path::string_type str;
      is >> str;
      ph = str;
      return is;
    }
# elif BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
    template< class String, class Traits >
    std::basic_ostream< BOOST_DEDUCED_TYPENAME String::value_type,
      BOOST_DEDUCED_TYPENAME String::traits_type > &
      operator<<
      ( std::basic_ostream< BOOST_DEDUCED_TYPENAME String::value_type,
          BOOST_DEDUCED_TYPENAME String::traits_type >& os, 
        const basic_path< String, Traits > & ph )
    {
      os << ph.string();
      return os;
    }

    template< class String, class Traits >
    std::basic_istream< BOOST_DEDUCED_TYPENAME String::value_type, 
      BOOST_DEDUCED_TYPENAME String::traits_type > &
      operator>>
      ( std::basic_istream< BOOST_DEDUCED_TYPENAME String::value_type,
          BOOST_DEDUCED_TYPENAME String::traits_type> & is,
        basic_path< String, Traits > & ph )
    {
      String str;
      is >> str;
      ph = str;
      return is;
    }
# endif

  //  path::name_checks  -----------------------------------------------------//

    BOOST_FILESYSTEM_DECL bool portable_posix_name( const std::string & name );
    BOOST_FILESYSTEM_DECL bool windows_name( const std::string & name );
    BOOST_FILESYSTEM_DECL bool portable_name( const std::string & name );
    BOOST_FILESYSTEM_DECL bool portable_directory_name( const std::string & name );
    BOOST_FILESYSTEM_DECL bool portable_file_name( const std::string & name );
    BOOST_FILESYSTEM_DECL bool native( const std::string & name );
    inline bool no_check( const std::string & )
      { return true; }

// implementation  -----------------------------------------------------------//

    namespace detail
    {

      //  is_separator helper ------------------------------------------------//

      template<class Path>
      inline  bool is_separator( typename Path::string_type::value_type c )
      {
        return c == slash<Path>::value
#     ifdef BOOST_WINDOWS_PATH
          || c == path_alt_separator<Path>::value
#     endif
          ;
      }

      // leaf_pos helper  ----------------------------------------------------//

      template<class String, class Traits>
      typename String::size_type leaf_pos(
        const String & str, // precondition: portable generic path grammar
        typename String::size_type end_pos ) // end_pos is past-the-end position
      // return 0 if str itself is leaf (or empty)
      {
        typedef typename
          boost::BOOST_FILESYSTEM_NAMESPACE::basic_path<String, Traits> path_type;

        // case: "//"
        if ( end_pos == 2 
          && str[0] == slash<path_type>::value
          && str[1] == slash<path_type>::value ) return 0;

        // case: ends in "/"
        if ( end_pos && str[end_pos-1] == slash<path_type>::value )
          return end_pos-1;
        
        // set pos to start of last element
        typename String::size_type pos(
          str.find_last_of( slash<path_type>::value, end_pos-1 ) );
#       ifdef BOOST_WINDOWS_PATH
        if ( pos == String::npos )
          pos = str.find_last_of( path_alt_separator<path_type>::value, end_pos-1 );
        if ( pos == String::npos )
          pos = str.find_last_of( colon<path_type>::value, end_pos-2 );
#       endif

        return ( pos == String::npos // path itself must be a leaf (or empty)
          || (pos == 1 && str[0] == slash<path_type>::value) ) // or net
            ? 0 // so leaf is entire string
            : pos + 1; // or starts after delimiter
      }

      // first_element helper  -----------------------------------------------//
      //   sets pos and len of first element, excluding extra separators
      //   if src.empty(), sets pos,len, to 0,0.

      template<class String, class Traits>
        void first_element(
          const String & src, // precondition: portable generic path grammar
          typename String::size_type & element_pos,
          typename String::size_type & element_size,
#       if !BOOST_WORKAROUND( BOOST_MSVC, <= 1310 ) // VC++ 7.1
          typename String::size_type size = String::npos
#       else
          typename String::size_type size = -1
#       endif
          )
      {
        if ( size == String::npos ) size = src.size();
        element_pos = 0;
        element_size = 0;
        if ( src.empty() ) return;

        typedef typename boost::BOOST_FILESYSTEM_NAMESPACE::basic_path<String, Traits> path_type;

        typename String::size_type cur(0);
        
        // deal with // [network]
        if ( size >= 2 && src[0] == slash<path_type>::value
          && src[1] == slash<path_type>::value
          && (size == 2
            || src[2] != slash<path_type>::value) )
        { 
          cur += 2;
          element_size += 2;
        }

        // leading (not non-network) separator
        else if ( src[0] == slash<path_type>::value )
        {
          ++element_size;
          // bypass extra leading separators
          while ( cur+1 < size
            && src[cur+1] == slash<path_type>::value )
          {
            ++cur;
            ++element_pos;
          }
          return;
        }

        // at this point, we have either a plain name, a network name,
        // or (on Windows only) a device name

        // find the end
        while ( cur < size
#         ifdef BOOST_WINDOWS_PATH
          && src[cur] != colon<path_type>::value
#         endif
          && src[cur] != slash<path_type>::value )
        {
          ++cur;
          ++element_size;
        }

#       ifdef BOOST_WINDOWS_PATH
        if ( cur == size ) return;
        // include device delimiter
        if ( src[cur] == colon<path_type>::value )
          { ++element_size; }
#       endif

        return;
      }

      // root_directory_start helper  ----------------------------------------//

      template<class String, class Traits>
      typename String::size_type root_directory_start(
        const String & s, // precondition: portable generic path grammar
        typename String::size_type size )
      // return npos if no root_directory found
      {
        typedef typename boost::BOOST_FILESYSTEM_NAMESPACE::basic_path<String, Traits> path_type;

#     ifdef BOOST_WINDOWS_PATH
        // case "c:/"
        if ( size > 2
          && s[1] == colon<path_type>::value
          && s[2] == slash<path_type>::value ) return 2;
#     endif

        // case "//"
        if ( size == 2
          && s[0] == slash<path_type>::value
          && s[1] == slash<path_type>::value ) return String::npos;

        // case "//net {/}"
        if ( size > 3
          && s[0] == slash<path_type>::value
          && s[1] == slash<path_type>::value
          && s[2] != slash<path_type>::value )
        {
          typename String::size_type pos(
            s.find( slash<path_type>::value, 2 ) );
          return pos < size ? pos : String::npos;
        }
        
        // case "/"
        if ( size > 0 && s[0] == slash<path_type>::value ) return 0;

        return String::npos;
      }

      // is_non_root_slash helper  -------------------------------------------//

      template<class String, class Traits>
      bool is_non_root_slash( const String & str,
        typename String::size_type pos ) // pos is position of the slash
      {
        typedef typename
          boost::BOOST_FILESYSTEM_NAMESPACE::basic_path<String, Traits>
            path_type;

        assert( !str.empty() && str[pos] == slash<path_type>::value
          && "precondition violation" );

        // subsequent logic expects pos to be for leftmost slash of a set
        while ( pos > 0 && str[pos-1] == slash<path_type>::value )
          --pos;

        return  pos != 0
          && (pos <= 2 || str[1] != slash<path_type>::value
            || str.find( slash<path_type>::value, 2 ) != pos)
#       ifdef BOOST_WINDOWS_PATH
          && (pos !=2 || str[1] != colon<path_type>::value)
#       endif
            ;
      }
    } // namespace detail

    // decomposition functions  ----------------------------------------------//

    template<class String, class Traits>
    String basic_path<String, Traits>::leaf() const
    {
      typename String::size_type end_pos(
        detail::leaf_pos<String, Traits>( m_path, m_path.size() ) );
      return (m_path.size()
                && end_pos
                && m_path[end_pos] == slash<path_type>::value
                && detail::is_non_root_slash< String, Traits >(m_path, end_pos))
        ? String( 1, dot<path_type>::value )
        : m_path.substr( end_pos );
    }

    template<class String, class Traits>
    basic_path<String, Traits> basic_path<String, Traits>::branch_path() const
    {
      typename String::size_type end_pos(
        detail::leaf_pos<String, Traits>( m_path, m_path.size() ) );

      bool leaf_was_separator( m_path.size()
        && m_path[end_pos] == slash<path_type>::value );

      // skip separators unless root directory
      typename string_type::size_type root_dir_pos( detail::root_directory_start
        <string_type, traits_type>( m_path, end_pos ) );
      for ( ; 
        end_pos > 0
        && (end_pos-1) != root_dir_pos
        && m_path[end_pos-1] == slash<path_type>::value
        ;
        --end_pos ) {}

     return (end_pos == 1 && root_dir_pos == 0 && leaf_was_separator)
       ? path_type()
       : path_type( m_path.substr( 0, end_pos ) );
    }

    template<class String, class Traits>
    basic_path<String, Traits> basic_path<String, Traits>::relative_path() const
    {
      iterator itr( begin() );
      for ( ; itr.m_pos != m_path.size()
          && (itr.m_name[0] == slash<path_type>::value
#     ifdef BOOST_WINDOWS_PATH
          || itr.m_name[itr.m_name.size()-1]
            == colon<path_type>::value
#     endif
             ); ++itr ) {}

      return basic_path<String, Traits>( m_path.substr( itr.m_pos ) );
    }

    template<class String, class Traits>
    String basic_path<String, Traits>::root_name() const
    {
      iterator itr( begin() );

      return ( itr.m_pos != m_path.size()
        && (
            ( itr.m_name.size() > 1
              && itr.m_name[0] == slash<path_type>::value
              && itr.m_name[1] == slash<path_type>::value
            )
#     ifdef BOOST_WINDOWS_PATH
          || itr.m_name[itr.m_name.size()-1]
            == colon<path_type>::value
#     endif
           ) )
        ? *itr
        : String();
    }

    template<class String, class Traits>
    String basic_path<String, Traits>::root_directory() const
    {
      typename string_type::size_type start(
        detail::root_directory_start<String, Traits>( m_path, m_path.size() ) );

      return start == string_type::npos
        ? string_type()
        : m_path.substr( start, 1 );
    }

    template<class String, class Traits>
    basic_path<String, Traits> basic_path<String, Traits>::root_path() const
    {
      // even on POSIX, root_name() is non-empty() on network paths
      return basic_path<String, Traits>( root_name() ) /= root_directory();
    }

    // path query functions  -------------------------------------------------//

    template<class String, class Traits>
    inline bool basic_path<String, Traits>::is_complete() const
    {
#   ifdef BOOST_WINDOWS_PATH
      return has_root_name() && has_root_directory();
#   else
      return has_root_directory();
#   endif
    }

    template<class String, class Traits>
    inline bool basic_path<String, Traits>::has_root_path() const
    {
      return !root_path().empty();
    }

    template<class String, class Traits>
    inline bool basic_path<String, Traits>::has_root_name() const
    {
      return !root_name().empty();
    }

    template<class String, class Traits>
    inline bool basic_path<String, Traits>::has_root_directory() const
    {
      return !root_directory().empty();
    }

    // append  ---------------------------------------------------------------//

    template<class String, class Traits>
    void basic_path<String, Traits>::m_append_separator_if_needed()
    // requires: !empty()
    {
      if (
#       ifdef BOOST_WINDOWS_PATH
        *(m_path.end()-1) != colon<path_type>::value && 
#       endif
        *(m_path.end()-1) != slash<path_type>::value )
      {
        m_path += slash<path_type>::value;
      }
    }
      
    template<class String, class Traits>
    void basic_path<String, Traits>::m_append( value_type value )
    {
#   ifdef BOOST_CYGWIN_PATH
      if ( m_path.empty() ) m_cygwin_root = (value == slash<path_type>::value);
#   endif

#   ifdef BOOST_WINDOWS_PATH
      // for BOOST_WINDOWS_PATH, convert alt_separator ('\') to separator ('/')
      m_path += ( value == path_alt_separator<path_type>::value
        ? slash<path_type>::value
        : value );
#   else
      m_path += value;
#   endif
    }
    
    // except that it wouldn't work for BOOST_NO_MEMBER_TEMPLATES compilers,
    // the append() member template could replace this code.
    template<class String, class Traits>
    basic_path<String, Traits> & basic_path<String, Traits>::operator /=
      ( const value_type * next_p )
    {
      // ignore escape sequence on POSIX or Windows
      if ( *next_p == slash<path_type>::value
        && *(next_p+1) == slash<path_type>::value
        && *(next_p+2) == colon<path_type>::value ) next_p += 3;
      
      // append slash<path_type>::value if needed
      if ( !empty() && *next_p != 0
        && !detail::is_separator<path_type>( *next_p ) )
      { m_append_separator_if_needed(); }

      for ( ; *next_p != 0; ++next_p ) m_append( *next_p );
      return *this;
    }

# ifndef BOOST_NO_MEMBER_TEMPLATES
    template<class String, class Traits> template <class InputIterator>
      basic_path<String, Traits> & basic_path<String, Traits>::append(
        InputIterator first, InputIterator last )
    {
      // append slash<path_type>::value if needed
      if ( !empty() && first != last
        && !detail::is_separator<path_type>( *first ) )
      { m_append_separator_if_needed(); }

      // song-and-dance to avoid violating InputIterator requirements
      // (which prohibit lookahead) in detecting a possible escape sequence
      // (escape sequences are simply ignored on POSIX and Windows)
      bool was_escape_sequence(true);
      std::size_t append_count(0);
      typename String::size_type initial_pos( m_path.size() );

      for ( ; first != last && *first; ++first )
      {
        if ( append_count == 0 && *first != slash<path_type>::value )
          was_escape_sequence = false;
        if ( append_count == 1 && *first != slash<path_type>::value )
          was_escape_sequence = false;
        if ( append_count == 2 && *first != colon<path_type>::value )
          was_escape_sequence = false;
        m_append( *first );
        ++append_count;
      }

      // erase escape sequence if any
      if ( was_escape_sequence && append_count >= 3 )
        m_path.erase( initial_pos, 3 );

      return *this;
    }
# endif

# ifndef BOOST_FILESYSTEM_NO_DEPRECATED

    // canonize  ------------------------------------------------------------//

    template<class String, class Traits>
    basic_path<String, Traits> & basic_path<String, Traits>::canonize()
    {
      static const typename string_type::value_type dot_str[]
        = { dot<path_type>::value, 0 };

      if ( m_path.empty() ) return *this;
        
      path_type temp;

      for ( iterator itr( begin() ); itr != end(); ++itr )
      {
        temp /= *itr;
      };

      if ( temp.empty() ) temp /= dot_str;
      m_path = temp.m_path;
      return *this;
    }

    // normalize  ------------------------------------------------------------//

    template<class String, class Traits>
    basic_path<String, Traits> & basic_path<String, Traits>::normalize()
    {
      static const typename string_type::value_type dot_str[]
        = { dot<path_type>::value, 0 };

      if ( m_path.empty() ) return *this;
        
      path_type temp;
      iterator start( begin() );
      iterator last( end() );
      iterator stop( last-- );
      for ( iterator itr( start ); itr != stop; ++itr )
      {
        // ignore "." except at start and last
        if ( itr->size() == 1
          && (*itr)[0] == dot<path_type>::value
          && itr != start
          && itr != last ) continue;

        // ignore a name and following ".."
        if ( !temp.empty()
          && itr->size() == 2
          && (*itr)[0] == dot<path_type>::value
          && (*itr)[1] == dot<path_type>::value ) // dot dot
        {
          string_type lf( temp.leaf() );  
          if ( lf.size() > 0  
            && (lf.size() != 1
              || (lf[0] != dot<path_type>::value
                && lf[0] != slash<path_type>::value))
            && (lf.size() != 2 
              || (lf[0] != dot<path_type>::value
                && lf[1] != dot<path_type>::value
#             ifdef BOOST_WINDOWS_PATH
                && lf[1] != colon<path_type>::value
#             endif
                 )
               )
            )
          {
            temp.remove_leaf();
            // if not root directory, must also remove "/" if any
            if ( temp.m_path.size() > 0
              && temp.m_path[temp.m_path.size()-1]
                == slash<path_type>::value )
            {
              typename string_type::size_type rds(
                detail::root_directory_start<String,Traits>( temp.m_path,
                  temp.m_path.size() ) );
              if ( rds == string_type::npos
                || rds != temp.m_path.size()-1 ) 
                { temp.m_path.erase( temp.m_path.size()-1 ); }
            }

            iterator next( itr );
            if ( temp.empty() && ++next != stop
              && next == last && *last == dot_str ) temp /= dot_str;
            continue;
          }
        }

        temp /= *itr;
      };

      if ( temp.empty() ) temp /= dot_str;
      m_path = temp.m_path;
      return *this;
    }

# endif

    // remove_leaf  ----------------------------------------------------------//

    template<class String, class Traits>
    basic_path<String, Traits> & basic_path<String, Traits>::remove_leaf()
    {
      m_path.erase(
        detail::leaf_pos<String, Traits>( m_path, m_path.size() ) );
      return *this;
    }

    // path conversion functions  --------------------------------------------//

    template<class String, class Traits>
    const String
    basic_path<String, Traits>::file_string() const
    {
#   ifdef BOOST_WINDOWS_PATH
      // for Windows, use the alternate separator, and bypass extra 
      // root separators

      typename string_type::size_type root_dir_start(
        detail::root_directory_start<String, Traits>( m_path, m_path.size() ) );
      bool in_root( root_dir_start != string_type::npos );
      String s;
      for ( typename string_type::size_type pos( 0 );
        pos != m_path.size(); ++pos )
      {
        // special case // [net]
        if ( pos == 0 && m_path.size() > 1
          && m_path[0] == slash<path_type>::value
          && m_path[1] == slash<path_type>::value
          && ( m_path.size() == 2 
            || !detail::is_separator<path_type>( m_path[2] )
             ) )
        {
          ++pos;
          s += path_alt_separator<path_type>::value;
          s += path_alt_separator<path_type>::value;
          continue;
        }   

        // bypass extra root separators
        if ( in_root )
        { 
          if ( s.size() > 0
            && s[s.size()-1] == path_alt_separator<path_type>::value
            && m_path[pos] == slash<path_type>::value
            ) continue;
        }

        if ( m_path[pos] == slash<path_type>::value )
          s += path_alt_separator<path_type>::value;
        else
          s += m_path[pos];

        if ( pos > root_dir_start
          && m_path[pos] == slash<path_type>::value )
          { in_root = false; }
      }
#   ifdef BOOST_CYGWIN_PATH
      if ( m_cygwin_root ) s[0] = slash<path_type>::value;
#   endif
      return s;
#   else
      return m_path;
#   endif
    }

    // iterator functions  ---------------------------------------------------//

    template<class String, class Traits>
    typename basic_path<String, Traits>::iterator basic_path<String, Traits>::begin() const
    {
      iterator itr;
      itr.m_path_ptr = this;
      typename string_type::size_type element_size;
      detail::first_element<String, Traits>( m_path, itr.m_pos, element_size );
      itr.m_name = m_path.substr( itr.m_pos, element_size );
      return itr;
    }

    template<class String, class Traits>
    typename basic_path<String, Traits>::iterator basic_path<String, Traits>::end() const
      {
        iterator itr;
        itr.m_path_ptr = this;
        itr.m_pos = m_path.size();
        return itr;
      }

    namespace detail
    {
      //  do_increment  ------------------------------------------------------//

      template<class Path>
      void iterator_helper<Path>::do_increment( iterator & itr )
      {
        typedef typename Path::string_type string_type;
        typedef typename Path::traits_type traits_type;

        assert( itr.m_pos < itr.m_path_ptr->m_path.size() && "basic_path::iterator increment past end()" );

        bool was_net( itr.m_name.size() > 2
          && itr.m_name[0] == slash<Path>::value
          && itr.m_name[1] == slash<Path>::value
          && itr.m_name[2] != slash<Path>::value );

        // increment to position past current element
        itr.m_pos += itr.m_name.size();

        // if end reached, create end iterator
        if ( itr.m_pos == itr.m_path_ptr->m_path.size() )
        {
          itr.m_name.erase( itr.m_name.begin(), itr.m_name.end() ); // VC++ 6.0 lib didn't supply clear() 
          return;
        }

        // process separator (Windows drive spec is only case not a separator)
        if ( itr.m_path_ptr->m_path[itr.m_pos] == slash<Path>::value )
        {
          // detect root directory
          if ( was_net
  #       ifdef BOOST_WINDOWS_PATH
            // case "c:/"
            || itr.m_name[itr.m_name.size()-1] == colon<Path>::value
  #       endif
             )
          {
            itr.m_name = slash<Path>::value;
            return;
          }

          // bypass separators
          while ( itr.m_pos != itr.m_path_ptr->m_path.size()
            && itr.m_path_ptr->m_path[itr.m_pos] == slash<Path>::value )
            { ++itr.m_pos; }

          // detect trailing separator, and treat it as ".", per POSIX spec
          if ( itr.m_pos == itr.m_path_ptr->m_path.size()
            && detail::is_non_root_slash< string_type, traits_type >(
                itr.m_path_ptr->m_path, itr.m_pos-1 ) ) 
          {
            --itr.m_pos;
            itr.m_name = dot<Path>::value;
            return;
          }
        }

        // get next element
        typename string_type::size_type end_pos(
          itr.m_path_ptr->m_path.find( slash<Path>::value, itr.m_pos ) );
        itr.m_name = itr.m_path_ptr->m_path.substr( itr.m_pos, end_pos - itr.m_pos );
      } 

      //  do_decrement  ------------------------------------------------------//

      template<class Path>
      void iterator_helper<Path>::do_decrement( iterator & itr )
      {                                                                                
        assert( itr.m_pos && "basic_path::iterator decrement past begin()"  );

        typedef typename Path::string_type string_type;
        typedef typename Path::traits_type traits_type;

        typename string_type::size_type end_pos( itr.m_pos );

        typename string_type::size_type root_dir_pos(
          detail::root_directory_start<string_type, traits_type>(
            itr.m_path_ptr->m_path, end_pos ) );

        // if at end and there was a trailing non-root '/', return "."
        if ( itr.m_pos == itr.m_path_ptr->m_path.size()
          && itr.m_path_ptr->m_path.size() > 1
          && itr.m_path_ptr->m_path[itr.m_pos-1] == slash<Path>::value
          && detail::is_non_root_slash< string_type, traits_type >(
               itr.m_path_ptr->m_path, itr.m_pos-1 ) 
           )
        {
          --itr.m_pos;
            itr.m_name = dot<Path>::value;
            return;
        }

        // skip separators unless root directory
        for ( 
          ; 
          end_pos > 0
          && (end_pos-1) != root_dir_pos
          && itr.m_path_ptr->m_path[end_pos-1] == slash<Path>::value
          ;
          --end_pos ) {}

        itr.m_pos = detail::leaf_pos<string_type, traits_type>
            ( itr.m_path_ptr->m_path, end_pos );
        itr.m_name = itr.m_path_ptr->m_path.substr( itr.m_pos, end_pos - itr.m_pos );
      }
    } // namespace detail

    //  basic_filesystem_error implementation --------------------------------//

    template<class Path>
    basic_filesystem_error<Path>::basic_filesystem_error(
      const std::string & what, system_error_type sys_err_code )
      : filesystem_error(what, sys_err_code)
    {
      try
      {
        m_imp_ptr.reset( new m_imp );
      }
      catch (...) { m_imp_ptr.reset(); }
    }

    template<class Path>
    basic_filesystem_error<Path>::basic_filesystem_error(
      const std::string & what, const path_type & path1,
      system_error_type sys_err_code )
      : filesystem_error(what, sys_err_code)
    {
      try
      {
        m_imp_ptr.reset( new m_imp );
        m_imp_ptr->m_path1 = path1;
      }
      catch (...) { m_imp_ptr.reset(); }
    }

    template<class Path>
    basic_filesystem_error<Path>::basic_filesystem_error(
      const std::string & what, const path_type & path1,
      const path_type & path2, system_error_type sys_err_code )
      : filesystem_error(what, sys_err_code)
    {
      try
      {
        m_imp_ptr.reset( new m_imp );
        m_imp_ptr->m_path1 = path1;
        m_imp_ptr->m_path2 = path2;
      }
      catch (...) { m_imp_ptr.reset(); }
    }

  } // namespace BOOST_FILESYSTEM_NAMESPACE
} // namespace boost

#include <boost/config/abi_suffix.hpp> // pops abi_prefix.hpp pragmas

#endif // BOOST_FILESYSTEM_PATH_HPP
