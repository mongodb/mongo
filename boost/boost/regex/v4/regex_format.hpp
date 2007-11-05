/*
 *
 * Copyright (c) 1998-2002
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE         regex_format.hpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Provides formatting output routines for search and replace
  *                operations.  Note this is an internal header file included
  *                by regex.hpp, do not include on its own.
  */

#ifndef BOOST_REGEX_FORMAT_HPP
#define BOOST_REGEX_FORMAT_HPP


namespace boost{

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

//
// Forward declaration:
//
   template <class BidiIterator, class Allocator = BOOST_DEDUCED_TYPENAME std::vector<sub_match<BidiIterator> >::allocator_type >
class match_results;

namespace re_detail{

//
// struct trivial_format_traits:
// defines minimum localisation support for formatting
// in the case that the actual regex traits is unavailable.
//
template <class charT>
struct trivial_format_traits
{
   typedef charT char_type;

   static std::ptrdiff_t length(const charT* p)
   {
      return global_length(p);
   }
   static charT tolower(charT c)
   {
      return ::boost::re_detail::global_lower(c);
   }
   static charT toupper(charT c)
   {
      return ::boost::re_detail::global_upper(c);
   }
   static int value(const charT c, int radix)
   {
      int result = global_value(c);
      return result >= radix ? -1 : result;
   }
   int toi(const charT*& p1, const charT* p2, int radix)const
   {
      return global_toi(p1, p2, radix, *this);
   }
};

template <class OutputIterator, class Results, class traits>
class basic_regex_formatter
{
public:
   typedef typename traits::char_type char_type;
   basic_regex_formatter(OutputIterator o, const Results& r, const traits& t)
      : m_traits(t), m_results(r), m_out(o), m_state(output_copy), m_have_conditional(false) {}
   OutputIterator format(const char_type* p1, const char_type* p2, match_flag_type f);
   OutputIterator format(const char_type* p1, match_flag_type f)
   {
      return format(p1, p1 + m_traits.length(p1), f);
   }
private:
   typedef typename Results::value_type sub_match_type;
   enum output_state
   {
      output_copy,
      output_next_lower,
      output_next_upper,
      output_lower,
      output_upper,
      output_none
   };

   void put(char_type c);
   void put(const sub_match_type& sub);
   void format_all();
   void format_perl();
   void format_escape();
   void format_conditional();
   void format_until_scope_end();

   const traits& m_traits;       // the traits class for localised formatting operations
   const Results& m_results;     // the match_results being used.
   OutputIterator m_out;         // where to send output.
   const char_type* m_position;  // format string, current position
   const char_type* m_end;       // format string end
   match_flag_type m_flags;      // format flags to use
   output_state    m_state;      // what to do with the next character
   bool            m_have_conditional; // we are parsing a conditional
private:
   basic_regex_formatter(const basic_regex_formatter&);
   basic_regex_formatter& operator=(const basic_regex_formatter&);
};

template <class OutputIterator, class Results, class traits>
OutputIterator basic_regex_formatter<OutputIterator, Results, traits>::format(const char_type* p1, const char_type* p2, match_flag_type f)
{
   m_position = p1;
   m_end = p2;
   m_flags = f;
   format_all();
   return m_out;
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::format_all()
{
   // over and over:
   while(m_position != m_end)
   {
      switch(*m_position)
      {
      case '&':
         if(m_flags & ::boost::regex_constants::format_sed)
         {
            ++m_position;
            put(m_results[0]);
            break;
         }
         put(*m_position++);
         break;
      case '\\':
         format_escape();
         break;
      case '(':
         if(m_flags & boost::regex_constants::format_all)
         {
            ++m_position;
            bool have_conditional = m_have_conditional;
            m_have_conditional = false;
            format_until_scope_end();
            m_have_conditional = have_conditional;
            if(m_position == m_end)
               return;
            BOOST_ASSERT(*m_position == static_cast<char_type>(')'));
            ++m_position;  // skip the closing ')'
            break;
         }
         put(*m_position);
         ++m_position;
         break;
      case ')':
         if(m_flags & boost::regex_constants::format_all)
         {
            return;
         }
         put(*m_position);
         ++m_position;
         break;
      case ':':
         if((m_flags & boost::regex_constants::format_all) && m_have_conditional)
         {
            return;
         }
         put(*m_position);
         ++m_position;
         break;
      case '?':
         if(m_flags & boost::regex_constants::format_all)
         {
            ++m_position;
            format_conditional();
            break;
         }
         put(*m_position);
         ++m_position;
         break;
      case '$':
         if((m_flags & format_sed) == 0)
         {
            format_perl();
            break;
         }
         // fall through, not a special character:
      default:
         put(*m_position);
         ++m_position;
         break;
      }
   }
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::format_perl()
{
   //
   // On entry *m_position points to a '$' character
   // output the information that goes with it:
   //
   BOOST_ASSERT(*m_position == '$');
   //
   // see if this is a trailing '$':
   //
   if(++m_position == m_end)
   {
      --m_position;
      put(*m_position);
      ++m_position;
      return;
   }
   //
   // OK find out what kind it is:
   //
   switch(*m_position)
   {
   case '&':
      ++m_position;
      put(this->m_results[0]);
      break;
   case '`':
      ++m_position;
      put(this->m_results.prefix());
      break;
   case '\'':
      ++m_position;
      put(this->m_results.suffix());
      break;
   case '$':
      put(*m_position++);
      break;
   default:
      // see if we have a number:
      {
         std::ptrdiff_t len = ::boost::re_detail::distance(m_position, m_end);
         len = (std::min)(static_cast<std::ptrdiff_t>(2), len);
         int v = m_traits.toi(m_position, m_position + len, 10);
         if(v < 0)
         {
            // leave the $ as is, and carry on:
            --m_position;
            put(*m_position);
            ++m_position;
            break;
         }
         // otherwise output sub v:
         put(this->m_results[v]);
      }
   }
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::format_escape()
{
   // skip the escape and check for trailing escape:
   if(++m_position == m_end)
   {
      put(static_cast<char_type>('\\'));
      return;
   }
   // now switch on the escape type:
   switch(*m_position)
   {
   case 'a':
      put(static_cast<char_type>('\a'));
      ++m_position;
      break;
   case 'f':
      put(static_cast<char_type>('\f'));
      ++m_position;
      break;
   case 'n':
      put(static_cast<char_type>('\n'));
      ++m_position;
      break;
   case 'r':
      put(static_cast<char_type>('\r'));
      ++m_position;
      break;
   case 't':
      put(static_cast<char_type>('\t'));
      ++m_position;
      break;
   case 'v':
      put(static_cast<char_type>('\v'));
      ++m_position;
      break;
   case 'x':
      if(++m_position == m_end)
      {
         put(static_cast<char_type>('x'));
         return;
      }
      // maybe have \x{ddd}
      if(*m_position == static_cast<char_type>('{'))
      {
         ++m_position;
         int val = m_traits.toi(m_position, m_end, 16);
         if(val < 0)
         {
            // invalid value treat everything as literals:
            put(static_cast<char_type>('x'));
            put(static_cast<char_type>('{'));
            return;
         }
         if(*m_position != static_cast<char_type>('}'))
         {
            while(*m_position != static_cast<char_type>('\\'))
               --m_position;
            ++m_position;
            put(*m_position++);
            return;
         }
         ++m_position;
         put(static_cast<char_type>(val));
         return;
      }
      else
      {
         std::ptrdiff_t len = ::boost::re_detail::distance(m_position, m_end);
         len = (std::min)(static_cast<std::ptrdiff_t>(2), len);
         int val = m_traits.toi(m_position, m_position + len, 16);
         if(val < 0)
         {
            --m_position;
            put(*m_position++);
            return;
         }
         put(static_cast<char_type>(val));
      }
      break;
   case 'c':
      if(++m_position == m_end)
      {
         --m_position;
         put(*m_position++);
         return;
      }
      put(static_cast<char_type>(*m_position++ % 32));
      break;
   case 'e':
      put(static_cast<char_type>(27));
      ++m_position;
      break;
   default:
      // see if we have a perl specific escape:
      if((m_flags & boost::regex_constants::format_sed) == 0)
      {
         bool breakout = false;
         switch(*m_position)
         {
         case 'l':
            ++m_position;
            m_state = output_next_lower;
            breakout = true;
            break;
         case 'L':
            ++m_position;
            m_state = output_lower;
            breakout = true;
            break;
         case 'u':
            ++m_position;
            m_state = output_next_upper;
            breakout = true;
            break;
         case 'U':
            ++m_position;
            m_state = output_upper;
            breakout = true;
            break;
         case 'E':
            ++m_position;
            m_state = output_copy;
            breakout = true;
            break;
         }
         if(breakout)
            break;
      }
      // see if we have a \n sed style backreference:
      int v = m_traits.toi(m_position, m_position+1, 10);
      if((v > 0) || ((v == 0) && (m_flags & ::boost::regex_constants::format_sed)))
      {
         put(m_results[v]);
         break;
      }
      else if(v == 0)
      {
         // octal ecape sequence:
         --m_position;
         std::ptrdiff_t len = ::boost::re_detail::distance(m_position, m_end);
         len = (std::min)(static_cast<std::ptrdiff_t>(4), len);
         v = m_traits.toi(m_position, m_position + len, 8);
         BOOST_ASSERT(v >= 0);
         put(static_cast<char_type>(v));
         break;
      }
      // Otherwise output the character "as is":
      put(*m_position++);
      break;
   }
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::format_conditional()
{
   if(m_position == m_end)
   {
      // oops trailing '?':
      put(static_cast<char_type>('?'));
      return;
   }
   std::ptrdiff_t len = ::boost::re_detail::distance(m_position, m_end);
   len = (std::min)(static_cast<std::ptrdiff_t>(2), len);
   int v = m_traits.toi(m_position, m_position + len, 10);
   if(v < 0)
   {
      // oops not a number:
      put(static_cast<char_type>('?'));
      return;
   }

   // output varies depending upon whether sub-expression v matched or not:
   if(m_results[v].matched)
   {
      m_have_conditional = true;
      format_all();
      m_have_conditional = false;
      if((m_position != m_end) && (*m_position == static_cast<char_type>(':')))
      {
         // skip the ':':
         ++m_position;
         // save output state, then turn it off:
         output_state saved_state = m_state;
         m_state = output_none;
         // format the rest of this scope:
         format_until_scope_end();
         // restore output state:
         m_state = saved_state;
      }
   }
   else
   {
      // save output state, then turn it off:
      output_state saved_state = m_state;
      m_state = output_none;
      // format until ':' or ')':
      m_have_conditional = true;
      format_all();
      m_have_conditional = false;
      // restore state:
      m_state = saved_state;
      if((m_position != m_end) && (*m_position == static_cast<char_type>(':')))
      {
         // skip the ':':
         ++m_position;
         // format the rest of this scope:
         format_until_scope_end();
      }
   }
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::format_until_scope_end()
{
   do
   {
      format_all();
      if((m_position == m_end) || (*m_position == static_cast<char_type>(')')))
         return;
      put(*m_position++);
   }while(m_position != m_end);
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::put(char_type c)
{
   // write a single character to output
   // according to which case translation mode we are in:
   switch(this->m_state)
   {
   case output_none:
      return;
   case output_next_lower:
      c = m_traits.tolower(c);
      this->m_state = output_copy;
      break;
   case output_next_upper:
      c = m_traits.toupper(c);
      this->m_state = output_copy;
      break;
   case output_lower:
      c = m_traits.tolower(c);
      break;
   case output_upper:
      c = m_traits.toupper(c);
      break;
   default:
      break;
   }
   *m_out = c;
   ++m_out;
}

template <class OutputIterator, class Results, class traits>
void basic_regex_formatter<OutputIterator, Results, traits>::put(const sub_match_type& sub)
{
   typedef typename sub_match_type::iterator iterator_type;
   iterator_type i = sub.first;
   while(i != sub.second)
   {
      put(*i);
      ++i;
   }
}

template <class S>
class string_out_iterator
#ifndef BOOST_NO_STD_ITERATOR
   : public std::iterator<std::output_iterator_tag, typename S::value_type>
#endif
{
   S* out;
public:
   string_out_iterator(S& s) : out(&s) {}
   string_out_iterator& operator++() { return *this; }
   string_out_iterator& operator++(int) { return *this; }
   string_out_iterator& operator*() { return *this; }
   string_out_iterator& operator=(typename S::value_type v) 
   { 
      out->append(1, v); 
      return *this; 
   }

#ifdef BOOST_NO_STD_ITERATOR
   typedef std::ptrdiff_t difference_type;
   typedef typename S::value_type value_type;
   typedef value_type* pointer;
   typedef value_type& reference;
   typedef std::output_iterator_tag iterator_category;
#endif
};

template <class OutputIterator, class Iterator, class Alloc, class charT, class traits>
OutputIterator regex_format_imp(OutputIterator out,
                          const match_results<Iterator, Alloc>& m,
                          const charT* p1, const charT* p2,
                          match_flag_type flags,
                          const traits& t
                         )
{
   if(flags & regex_constants::format_literal)
   {
      return re_detail::copy(p1, p2, out);
   }

   re_detail::basic_regex_formatter<
      OutputIterator, 
      match_results<Iterator, Alloc>, 
      traits > f(out, m, t);
   return f.format(p1, p2, flags);
}


} // namespace re_detail

template <class OutputIterator, class Iterator, class charT>
OutputIterator regex_format(OutputIterator out,
                          const match_results<Iterator>& m,
                          const charT* fmt,
                          match_flag_type flags = format_all
                         )
{
   re_detail::trivial_format_traits<charT> traits;
   return re_detail::regex_format_imp(out, m, fmt, fmt + traits.length(fmt), flags, traits);
}

template <class OutputIterator, class Iterator, class charT>
OutputIterator regex_format(OutputIterator out,
                          const match_results<Iterator>& m,
                          const std::basic_string<charT>& fmt,
                          match_flag_type flags = format_all
                         )
{
   re_detail::trivial_format_traits<charT> traits;
   return re_detail::regex_format_imp(out, m, fmt.data(), fmt.data() + fmt.size(), flags, traits);
}  

template <class Iterator, class charT>
std::basic_string<charT> regex_format(const match_results<Iterator>& m, 
                                      const charT* fmt, 
                                      match_flag_type flags = format_all)
{
   std::basic_string<charT> result;
   re_detail::string_out_iterator<std::basic_string<charT> > i(result);
   re_detail::trivial_format_traits<charT> traits;
   re_detail::regex_format_imp(i, m, fmt, fmt + traits.length(fmt), flags, traits);
   return result;
}

template <class Iterator, class charT>
std::basic_string<charT> regex_format(const match_results<Iterator>& m, 
                                      const std::basic_string<charT>& fmt, 
                                      match_flag_type flags = format_all)
{
   std::basic_string<charT> result;
   re_detail::string_out_iterator<std::basic_string<charT> > i(result);
   re_detail::trivial_format_traits<charT> traits;
   re_detail::regex_format_imp(i, m, fmt.data(), fmt.data() + fmt.size(), flags, traits);
   return result;
}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

} // namespace boost

#endif  // BOOST_REGEX_FORMAT_HPP






