//  Boost lpath.hpp  ---------------------------------------------------------//

//  Copyright Beman Dawes 2005

//  Use, modification, and distribution is subject to the Boost Software
//  License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See library home page at http://www.boost.org/libs/filesystem

#include <boost/filesystem/v2/path.hpp>
#include <cwchar>      // for std::mbstate_t
#include <string>
#include <ios>         // for std::streamoff

namespace std
{
  // Note well: this specialization is meant only to support wide_test.cpp.
  // It is not fully functional, fully correct, or efficient.
  template<> struct char_traits<long>
  {
    typedef long char_type;
    typedef long int_type;
    typedef streamoff off_type;
    typedef streampos pos_type;
    typedef mbstate_t state_type;
    static void assign(char_type& c1, const char_type& c2){c1=c2;}
    static bool eq(const char_type& c1, const char_type& c2){return c1==c2;}
    static bool lt(const char_type& c1, const char_type& c2){return c1<c2;}
    static int compare(const char_type* s1, const char_type* s2, size_t n)
    {
      const char_type* e = s1 + n;
      for ( ;s1 != e && *s1 == *s2; ++s1, ++s2 ) {}
      return s1 == e ? 0 : (*s1<*s2 ? -1 : 1);
    }
    static size_t length(const char_type* s)
    { const char_type* b=s; for(;*s!=0L;++s){} return s-b; } 
 
    static const char_type* find(const char_type* /*s*/, size_t /*n*/, const char_type& /*a*/)
    {   return 0; }

    // copy semantics will do for wide_test
    static char_type* move(char_type* s1, const char_type* s2, size_t n)
      { char_type* b=s1; for(const char_type* e=s1+n;s1!=e;++s1,++s2) *s1=*s2; return b; }

    static char_type* copy(char_type* s1, const char_type* s2, size_t n)
      { char_type* b=s1; for(const char_type* e=s1+n;s1!=e;++s1,++s2) *s1=*s2; return b; }

    static char_type* assign(char_type* s, size_t n, char_type a)
      { char_type* b=s; for(char_type* e=s+n;s!=e;++s) *s=a; return b; }
 
    static int_type not_eof(const int_type& c);
    static char_type to_char_type(const int_type& c);
    static int_type to_int_type(const char_type& c);
    static bool eq_int_type(const int_type& c1, const int_type& c2);
    static int_type eof();
  };
}

namespace user
{
  typedef std::basic_string<long> lstring;
  struct lpath_traits;
  typedef boost::filesystem::basic_path<lstring, lpath_traits> lpath;

  struct lpath_traits
  {
    typedef lstring internal_string_type;
    typedef std::string external_string_type;

    static external_string_type to_external( const lpath &,
      const internal_string_type & src )
    {
      external_string_type tmp;
      for ( internal_string_type::const_iterator it( src.begin() );
        it != src.end(); ++it )
      {
        tmp += static_cast<external_string_type::value_type>(*it);
      }
      return tmp;
    }

    static internal_string_type to_internal( const external_string_type & src )
    {
      internal_string_type tmp;
      for ( external_string_type::const_iterator it( src.begin() );
        it != src.end(); ++it ) tmp += *it;
      return tmp;
    }
  };

} // namespace user

namespace boost
{
  namespace filesystem2
  {
    template<> struct is_basic_path<user::lpath>
      { static const bool value = true; };
  }
}
