// tuple_io.hpp --------------------------------------------------------------

// Copyright (C) 2001 Jaakko Järvi (jaakko.jarvi@cs.utu.fi)
//               2001 Gary Powell (gary.powell@sierra.com)
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// For more information, see http://www.boost.org 

// ----------------------------------------------------------------------------

#ifndef BOOST_TUPLE_IO_HPP
#define BOOST_TUPLE_IO_HPP


// add to boost/config.hpp
// for now
# if defined __GNUC__
#   if (__GNUC__ == 2 && __GNUC_MINOR__ <= 97) 
#define BOOST_NO_TEMPLATED_STREAMS
#endif
#endif // __GNUC__

#if defined BOOST_NO_TEMPLATED_STREAMS
#include <iostream>
#else 
#include <istream>
#include <ostream>
#endif  

#include "boost/tuple/tuple.hpp"

// This is ugly: one should be using twoargument isspace since whitspace can
// be locale dependent, in theory at least.
// not all libraries implement have the two-arg version, so we need to 
// use the one-arg one, which one should get with <cctype> but there seem
// to be exceptions to this.

#if !defined (BOOST_NO_STD_LOCALE)

#include <locale> // for two-arg isspace

#else 

#include <cctype> // for one-arg (old) isspace 
#include <ctype.h> // Metrowerks does not find one-arg isspace from cctype

#endif

namespace boost {
namespace tuples {

namespace detail {

class format_info {
public:   

   enum manipulator_type { open, close, delimiter };
   BOOST_STATIC_CONSTANT(int, number_of_manipulators = delimiter + 1);
private:
   
   static int get_stream_index (int m)
   {
     static const int stream_index[number_of_manipulators]
        = { std::ios::xalloc(), std::ios::xalloc(), std::ios::xalloc() };

     return stream_index[m];
   }

   format_info(const format_info&);
   format_info();   


public:

#if defined (BOOST_NO_TEMPLATED_STREAMS)
   static char get_manipulator(std::ios& i, manipulator_type m) {
     char c = static_cast<char>(i.iword(get_stream_index(m))); 
     
     // parentheses and space are the default manipulators
     if (!c) {
       switch(m) {
         case detail::format_info::open : c = '('; break;
         case detail::format_info::close : c = ')'; break;
         case detail::format_info::delimiter : c = ' '; break;
       }
     }
     return c;
   }

   static void set_manipulator(std::ios& i, manipulator_type m, char c) {
      i.iword(get_stream_index(m)) = static_cast<long>(c);
   }
#else
   template<class CharType, class CharTrait>
   static CharType get_manipulator(std::basic_ios<CharType, CharTrait>& i, 
                                   manipulator_type m) {
     // The manipulators are stored as long.
     // A valid instanitation of basic_stream allows CharType to be any POD,
     // hence, the static_cast may fail (it fails if long is not convertible 
     // to CharType
     CharType c = static_cast<CharType>(i.iword(get_stream_index(m)) ); 
     // parentheses and space are the default manipulators
     if (!c) {
       switch(m) {
         case detail::format_info::open :  c = i.widen('('); break;
         case detail::format_info::close : c = i.widen(')'); break;
         case detail::format_info::delimiter : c = i.widen(' '); break;
       }
     }
     return c;
   }


   template<class CharType, class CharTrait>
   static void set_manipulator(std::basic_ios<CharType, CharTrait>& i, 
                               manipulator_type m, CharType c) {
     // The manipulators are stored as long.
     // A valid instanitation of basic_stream allows CharType to be any POD,
     // hence, the static_cast may fail (it fails if CharType is not 
     // convertible long.
      i.iword(get_stream_index(m)) = static_cast<long>(c);
   }
#endif // BOOST_NO_TEMPLATED_STREAMS
};

} // end of namespace detail
 
template<class CharType>    
class tuple_manipulator {
  const detail::format_info::manipulator_type mt;
  CharType f_c;
public:
  explicit tuple_manipulator(detail::format_info::manipulator_type m, 
                             const char c = 0)
     : mt(m), f_c(c) {}
  
#if defined (BOOST_NO_TEMPLATED_STREAMS)
  void set(std::ios &io) const {
     detail::format_info::set_manipulator(io, mt, f_c);
  }
#else
#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
   template<class CharType2, class CharTrait>
  void set(std::basic_ios<CharType2, CharTrait> &io) const {
     detail::format_info::set_manipulator(io, mt, f_c);
  }
#else
   template<class CharTrait>
  void set(std::basic_ios<CharType, CharTrait> &io) const {
     detail::format_info::set_manipulator(io, mt, f_c);
  }
#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#endif // BOOST_NO_TEMPLATED_STREAMS
};

#if defined (BOOST_NO_TEMPLATED_STREAMS)
inline std::ostream&
operator<<(std::ostream& o, const tuple_manipulator<char>& m) {
  m.set(o);
  return o;
}

inline std::istream&
operator>>(std::istream& i, const tuple_manipulator<char>& m) {
  m.set(i);
  return i;
}

#else

template<class CharType, class CharTrait>
inline std::basic_ostream<CharType, CharTrait>&
operator<<(std::basic_ostream<CharType, CharTrait>& o, const tuple_manipulator<CharType>& m) {
  m.set(o);
  return o;
}

template<class CharType, class CharTrait>
inline std::basic_istream<CharType, CharTrait>&
operator>>(std::basic_istream<CharType, CharTrait>& i, const tuple_manipulator<CharType>& m) {
  m.set(i);
  return i;
}

#endif   // BOOST_NO_TEMPLATED_STREAMS
   
template<class CharType>
inline tuple_manipulator<CharType> set_open(const CharType c) {
   return tuple_manipulator<CharType>(detail::format_info::open, c);
}

template<class CharType>
inline tuple_manipulator<CharType> set_close(const CharType c) {
   return tuple_manipulator<CharType>(detail::format_info::close, c);
}

template<class CharType>
inline tuple_manipulator<CharType> set_delimiter(const CharType c) {
   return tuple_manipulator<CharType>(detail::format_info::delimiter, c);
}



   
   
// -------------------------------------------------------------
// printing tuples to ostream in format (a b c)
// parentheses and space are defaults, but can be overriden with manipulators
// set_open, set_close and set_delimiter
   
namespace detail {

// Note: The order of the print functions is critical 
// to let a conforming compiler  find and select the correct one.

#if defined (BOOST_NO_TEMPLATED_STREAMS)

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
template<class T1>
inline std::ostream& print(std::ostream& o, const cons<T1, null_type>& t) {
  return o << t.head;
}
#endif // BOOST_NO_TEMPLATED_STREAMS
 
inline std::ostream& print(std::ostream& o, const null_type&) { return o; }

template<class T1, class T2>
inline std::ostream& 
print(std::ostream& o, const cons<T1, T2>& t) {
  
  const char d = format_info::get_manipulator(o, format_info::delimiter);
   
  o << t.head;

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
  if (tuples::length<T2>::value == 0)
    return o;
#endif  // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  o << d;
  
  return print(o, t.tail );

}



#else

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
template<class CharType, class CharTrait, class T1>
inline std::basic_ostream<CharType, CharTrait>& 
print(std::basic_ostream<CharType, CharTrait>& o, const cons<T1, null_type>& t) {
  return o << t.head;
}
#endif  // !BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

 
template<class CharType, class CharTrait>
inline std::basic_ostream<CharType, CharTrait>& 
print(std::basic_ostream<CharType, CharTrait>& o, const null_type&) { 
  return o; 
}

template<class CharType, class CharTrait, class T1, class T2>
inline std::basic_ostream<CharType, CharTrait>& 
print(std::basic_ostream<CharType, CharTrait>& o, const cons<T1, T2>& t) {
  
  const CharType d = format_info::get_manipulator(o, format_info::delimiter);
   
  o << t.head;

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
  if (tuples::length<T2>::value == 0)
    return o;
#endif  // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
  o << d;

  return print(o, t.tail);
}

#endif  // BOOST_NO_TEMPLATED_STREAMS

} // namespace detail

#if defined (BOOST_NO_TEMPLATED_STREAMS)
template<class T1, class T2>
inline std::ostream& operator<<(std::ostream& o, const cons<T1, T2>& t) {
  if (!o.good() ) return o;
 
  const char l = 
    detail::format_info::get_manipulator(o, detail::format_info::open);
  const char r = 
    detail::format_info::get_manipulator(o, detail::format_info::close);
   
  o << l;
  
  detail::print(o, t);  

  o << r;

  return o;
}

#else

template<class CharType, class CharTrait, class T1, class T2>
inline std::basic_ostream<CharType, CharTrait>& 
operator<<(std::basic_ostream<CharType, CharTrait>& o, 
           const cons<T1, T2>& t) {
  if (!o.good() ) return o;
 
  const CharType l = 
    detail::format_info::get_manipulator(o, detail::format_info::open);
  const CharType r = 
    detail::format_info::get_manipulator(o, detail::format_info::close);
   
  o << l;   

  detail::print(o, t);  

  o << r;

  return o;
}
#endif // BOOST_NO_TEMPLATED_STREAMS

   
// -------------------------------------------------------------
// input stream operators

namespace detail {

#if defined (BOOST_NO_TEMPLATED_STREAMS)

inline std::istream& 
extract_and_check_delimiter(
  std::istream& is, format_info::manipulator_type del)
{
  const char d = format_info::get_manipulator(is, del);

#if defined (BOOST_NO_STD_LOCALE)
  const bool is_delimiter = !isspace(d);
#else
  const bool is_delimiter = (!std::isspace(d, is.getloc()) );            
#endif

  char c;
  if (is_delimiter) { 
    is >> c; 
    if (is.good() && c!=d) {
      is.setstate(std::ios::failbit);
    } 
  }
  return is;
}


// Note: The order of the read functions is critical to let a 
// (conforming?) compiler find and select the correct one.

#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
template<class T1>
inline  std::istream & 
read (std::istream &is, cons<T1, null_type>& t1) {

  if (!is.good()) return is;   
   
  return is >> t1.head ;
}
#else
inline std::istream& read(std::istream& i, const null_type&) { return i; }
#endif // !BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
   
template<class T1, class T2>
inline std::istream& 
read(std::istream &is, cons<T1, T2>& t1) {

  if (!is.good()) return is;
   
  is >> t1.head;

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
  if (tuples::length<T2>::value == 0)
    return is;
#endif  // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

  extract_and_check_delimiter(is, format_info::delimiter);

  return read(is, t1.tail);
}

} // end namespace detail

inline std::istream& 
operator>>(std::istream &is, null_type&) {

  if (!is.good() ) return is;

  detail::extract_and_check_delimiter(is, detail::format_info::open);
  detail::extract_and_check_delimiter(is, detail::format_info::close);

  return is;
}


template<class T1, class T2>
inline std::istream& 
operator>>(std::istream& is, cons<T1, T2>& t1) {

  if (!is.good() ) return is;

  detail::extract_and_check_delimiter(is, detail::format_info::open);
                      
  detail::read(is, t1);
   
  detail::extract_and_check_delimiter(is, detail::format_info::close);

  return is;
}



#else

template<class CharType, class CharTrait>
inline std::basic_istream<CharType, CharTrait>& 
extract_and_check_delimiter(
  std::basic_istream<CharType, CharTrait> &is, format_info::manipulator_type del)
{
  const CharType d = format_info::get_manipulator(is, del);

#if defined (BOOST_NO_STD_LOCALE)
  const bool is_delimiter = !isspace(d);
#elif defined ( __BORLANDC__ )
  const bool is_delimiter = !std::use_facet< std::ctype< CharType > >
    (is.getloc() ).is( std::ctype_base::space, d);
#else
  const bool is_delimiter = (!std::isspace(d, is.getloc()) );            
#endif

  CharType c;
  if (is_delimiter) { 
    is >> c;
    if (is.good() && c!=d) { 
      is.setstate(std::ios::failbit);
    }
  }
  return is;
}

   
#if !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
template<class CharType, class CharTrait, class T1>
inline  std::basic_istream<CharType, CharTrait> & 
read (std::basic_istream<CharType, CharTrait> &is, cons<T1, null_type>& t1) {

  if (!is.good()) return is;   
   
  return is >> t1.head; 
}
#else
template<class CharType, class CharTrait>
inline std::basic_istream<CharType, CharTrait>& 
read(std::basic_istream<CharType, CharTrait>& i, const null_type&) { return i; }

#endif // !BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

template<class CharType, class CharTrait, class T1, class T2>
inline std::basic_istream<CharType, CharTrait>& 
read(std::basic_istream<CharType, CharTrait> &is, cons<T1, T2>& t1) {

  if (!is.good()) return is;
   
  is >> t1.head;

#if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
  if (tuples::length<T2>::value == 0)
    return is;
#endif  // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

  extract_and_check_delimiter(is, format_info::delimiter);

  return read(is, t1.tail);
}

} // end namespace detail


template<class CharType, class CharTrait>
inline std::basic_istream<CharType, CharTrait>& 
operator>>(std::basic_istream<CharType, CharTrait> &is, null_type&) {

  if (!is.good() ) return is;

  detail::extract_and_check_delimiter(is, detail::format_info::open);
  detail::extract_and_check_delimiter(is, detail::format_info::close);

  return is;
}

template<class CharType, class CharTrait, class T1, class T2>
inline std::basic_istream<CharType, CharTrait>& 
operator>>(std::basic_istream<CharType, CharTrait>& is, cons<T1, T2>& t1) {

  if (!is.good() ) return is;

  detail::extract_and_check_delimiter(is, detail::format_info::open);
                      
  detail::read(is, t1);
   
  detail::extract_and_check_delimiter(is, detail::format_info::close);

  return is;
}

#endif // BOOST_NO_TEMPLATED_STREAMS

} // end of namespace tuples
} // end of namespace boost

#endif // BOOST_TUPLE_IO_HPP


