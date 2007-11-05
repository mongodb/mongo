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
  *   FILE         match_results.cpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Declares template class match_results.
  */

#ifndef BOOST_REGEX_V4_MATCH_RESULTS_HPP
#define BOOST_REGEX_V4_MATCH_RESULTS_HPP

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost{
#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable : 4251 4231 4660)
#endif

template <class BidiIterator, class Allocator>
class match_results
{ 
private:
#ifndef BOOST_NO_STD_ALLOCATOR
   typedef          std::vector<sub_match<BidiIterator>, Allocator> vector_type;
#else
   typedef          std::vector<sub_match<BidiIterator> >           vector_type;
#endif
public: 
   typedef          sub_match<BidiIterator>                         value_type;
#if  !defined(BOOST_NO_STD_ALLOCATOR) && !(defined(BOOST_MSVC) && defined(_STLPORT_VERSION))
   typedef typename Allocator::const_reference                              const_reference;
#else
   typedef          const value_type&                                       const_reference;
#endif
   typedef          const_reference                                         reference;
   typedef typename vector_type::const_iterator                             const_iterator;
   typedef          const_iterator                                          iterator;
   typedef typename re_detail::regex_iterator_traits<
                                    BidiIterator>::difference_type          difference_type;
   typedef typename Allocator::size_type                                    size_type;
   typedef          Allocator                                               allocator_type;
   typedef typename re_detail::regex_iterator_traits<
                                    BidiIterator>::value_type               char_type;
   typedef          std::basic_string<char_type>                            string_type;

   // construct/copy/destroy:
   explicit match_results(const Allocator& a = Allocator())
#ifndef BOOST_NO_STD_ALLOCATOR
      : m_subs(a), m_base() {}
#else
      : m_subs(), m_base() { (void)a; }
#endif
   match_results(const match_results& m)
      : m_subs(m.m_subs), m_base(m.m_base) {}
   match_results& operator=(const match_results& m)
   {
      m_subs = m.m_subs;
      m_base = m.m_base;
      return *this;
   }
   ~match_results(){}

   // size:
   size_type size() const
   { return empty() ? 0 : m_subs.size() - 2; }
   size_type max_size() const
   { return m_subs.max_size(); }
   bool empty() const
   { return m_subs.size() < 2; }
   // element access:
   difference_type length(int sub = 0) const
   {
      sub += 2;
      if((sub < (int)m_subs.size()) && (sub > 0))
         return m_subs[sub].length();
      return 0;
   }
   difference_type position(size_type sub = 0) const
   {
      sub += 2;
      if(sub < m_subs.size())
      {
         const sub_match<BidiIterator>& s = m_subs[sub];
         if(s.matched || (sub == 2))
         {
            return ::boost::re_detail::distance((BidiIterator)(m_base), (BidiIterator)(s.first));
         }
      }
      return ~static_cast<difference_type>(0);
   }
   string_type str(int sub = 0) const
   {
      sub += 2;
      string_type result;
      if(sub < (int)m_subs.size() && (sub > 0))
      {
         const sub_match<BidiIterator>& s = m_subs[sub];
         if(s.matched)
         {
            result = s.str();
         }
      }
      return result;
   }
   const_reference operator[](int sub) const
   {
      sub += 2;
      if(sub < (int)m_subs.size() && (sub >= 0))
      {
         return m_subs[sub];
      }
      return m_null;
   }

   const_reference prefix() const
   {
      return (*this)[-1];
   }

   const_reference suffix() const
   {
      return (*this)[-2];
   }
   const_iterator begin() const
   {
      return (m_subs.size() > 2) ? (m_subs.begin() + 2) : m_subs.end();
   }
   const_iterator end() const
   {
      return m_subs.end();
   }
   // format:
   template <class OutputIterator>
   OutputIterator format(OutputIterator out,
                         const string_type& fmt,
                         match_flag_type flags = format_default) const
   {
      re_detail::trivial_format_traits<char_type> traits;
      return re_detail::regex_format_imp(out, *this, fmt.data(), fmt.data() + fmt.size(), flags, traits);
   }
   string_type format(const string_type& fmt,
                      match_flag_type flags = format_default) const
   {
      string_type result;
      re_detail::string_out_iterator<string_type> i(result);
      re_detail::trivial_format_traits<char_type> traits;
      re_detail::regex_format_imp(i, *this, fmt.data(), fmt.data() + fmt.size(), flags, traits);
      return result;
   }
   // format with locale:
   template <class OutputIterator, class RegexT>
   OutputIterator format(OutputIterator out,
                         const string_type& fmt,
                         match_flag_type flags,
                         const RegexT& re) const
   {
      return ::boost::re_detail::regex_format_imp(out, *this, fmt.data(), fmt.data() + fmt.size(), flags, re.get_traits());
   }
   template <class RegexT>
   string_type format(const string_type& fmt,
                      match_flag_type flags,
                      const RegexT& re) const
   {
      string_type result;
      re_detail::string_out_iterator<string_type> i(result);
      ::boost::re_detail::regex_format_imp(i, *this, fmt.data(), fmt.data() + fmt.size(), flags, re.get_traits());
      return result;
   }

   allocator_type get_allocator() const
   {
#ifndef BOOST_NO_STD_ALLOCATOR
      return m_subs.get_allocator();
#else
     return allocator_type();
#endif
   }
   void swap(match_results& that)
   {
      std::swap(m_subs, that.m_subs);
      std::swap(m_base, that.m_base);
   }
   bool operator==(const match_results& that)const
   {
      return (m_subs == that.m_subs) && (m_base == that.m_base);
   }
   bool operator!=(const match_results& that)const
   { return !(*this == that); }

#ifdef BOOST_REGEX_MATCH_EXTRA
   typedef typename sub_match<BidiIterator>::capture_sequence_type capture_sequence_type;

   const capture_sequence_type& captures(int i)const
   {
      return (*this)[i].captures();
   }
#endif

   //
   // private access functions:
   void BOOST_REGEX_CALL set_second(BidiIterator i)
   {
      BOOST_ASSERT(m_subs.size() > 2);
      m_subs[2].second = i;
      m_subs[2].matched = true;
      m_subs[0].first = i;
      m_subs[0].matched = (m_subs[0].first != m_subs[0].second);
      m_null.first = i;
      m_null.second = i;
      m_null.matched = false;
   }

   void BOOST_REGEX_CALL set_second(BidiIterator i, size_type pos, bool m = true)
   {
      pos += 2;
      BOOST_ASSERT(m_subs.size() > pos);
      m_subs[pos].second = i;
      m_subs[pos].matched = m;
      if(pos == 2)
      {
         m_subs[0].first = i;
         m_subs[0].matched = (m_subs[0].first != m_subs[0].second);
         m_null.first = i;
         m_null.second = i;
         m_null.matched = false;
      }
   }
   void BOOST_REGEX_CALL set_size(size_type n, BidiIterator i, BidiIterator j)
   {
      value_type v(j);
      size_type len = m_subs.size();
      if(len > n + 2)
      {
         m_subs.erase(m_subs.begin()+n+2, m_subs.end());
         std::fill(m_subs.begin(), m_subs.end(), v);
      }
      else
      {
         std::fill(m_subs.begin(), m_subs.end(), v);
         if(n+2 != len)
            m_subs.insert(m_subs.end(), n+2-len, v);
      }
      m_subs[1].first = i;
   }
   void BOOST_REGEX_CALL set_base(BidiIterator pos)
   {
      m_base = pos;
   }
   BidiIterator base()const
   {
      return m_base;
   }
   void BOOST_REGEX_CALL set_first(BidiIterator i)
   {
      // set up prefix:
      m_subs[1].second = i;
      m_subs[1].matched = (m_subs[1].first != i);
      // set up $0:
      m_subs[2].first = i;
      // zero out everything else:
      for(size_type n = 3; n < m_subs.size(); ++n)
      {
         m_subs[n].first = m_subs[n].second = m_subs[0].second;
         m_subs[n].matched = false;
      }
   }
   void BOOST_REGEX_CALL set_first(BidiIterator i, size_type pos)
   {
      BOOST_ASSERT(pos+2 < m_subs.size());
      if(pos)
         m_subs[pos+2].first = i;
      else
         set_first(i);
   }
   void BOOST_REGEX_CALL maybe_assign(const match_results<BidiIterator, Allocator>& m);


private:
   vector_type            m_subs; // subexpressions
   BidiIterator   m_base; // where the search started from
   sub_match<BidiIterator> m_null; // a null match
};

template <class BidiIterator, class Allocator>
void BOOST_REGEX_CALL match_results<BidiIterator, Allocator>::maybe_assign(const match_results<BidiIterator, Allocator>& m)
{
   const_iterator p1, p2;
   p1 = begin();
   p2 = m.begin();
   //
   // Distances are measured from the start of *this* match, unless this isn't
   // a valid match in which case we use the start of the whole sequence.  Note that
   // no subsequent match-candidate can ever be to the left of the first match found.
   // This ensures that when we are using bidirectional iterators, that distances 
   // measured are as short as possible, and therefore as efficient as possible
   // to compute.  Finally note that we don't use the "matched" data member to test
   // whether a sub-expression is a valid match, because partial matches set this
   // to false for sub-expression 0.
   //
   BidiIterator l_end = this->suffix().second;
   BidiIterator l_base = (p1->first == l_end) ? this->prefix().first : (*this)[0].first;
   difference_type len1 = 0;
   difference_type len2 = 0;
   difference_type base1 = 0;
   difference_type base2 = 0;
   std::size_t i;
   for(i = 0; i < size(); ++i, ++p1, ++p2)
   {
      //
      // Leftmost takes priority over longest; handle special cases
      // where distances need not be computed first (an optimisation
      // for bidirectional iterators: ensure that we don't accidently
      // compute the length of the whole sequence, as this can be really
      // expensive).
      //
      if(p1->first == l_end)
      {
         if(p2->first != l_end)
         {
            // p2 must be better than p1, and no need to calculate
            // actual distances:
            base1 = 1;
            base2 = 0;
            break;
         }
         else
         {
            // *p1 and *p2 are either unmatched or match end-of sequence,
            // either way no need to calculate distances:
            if((p1->matched == false) && (p2->matched == true))
               break;
            if((p1->matched == true) && (p2->matched == false))
               return;
            continue;
         }
      }
      else if(p2->first == l_end)
      {
         // p1 better than p2, and no need to calculate distances:
         return;
      }
      base1 = ::boost::re_detail::distance(l_base, p1->first);
      base2 = ::boost::re_detail::distance(l_base, p2->first);
      BOOST_ASSERT(base1 >= 0);
      BOOST_ASSERT(base2 >= 0);
      if(base1 < base2) return;
      if(base2 < base1) break;

      len1 = ::boost::re_detail::distance((BidiIterator)p1->first, (BidiIterator)p1->second);
      len2 = ::boost::re_detail::distance((BidiIterator)p2->first, (BidiIterator)p2->second);
      BOOST_ASSERT(len1 >= 0);
      BOOST_ASSERT(len2 >= 0);
      if((len1 != len2) || ((p1->matched == false) && (p2->matched == true)))
         break;
      if((p1->matched == true) && (p2->matched == false))
         return;
   }
   if(i == size())
      return;
   if(base2 < base1)
      *this = m;
   else if((len2 > len1) || ((p1->matched == false) && (p2->matched == true)) )
      *this = m;
}

template <class BidiIterator, class Allocator>
void swap(match_results<BidiIterator, Allocator>& a, match_results<BidiIterator, Allocator>& b)
{
   a.swap(b);
}

#ifndef BOOST_NO_STD_LOCALE
template <class charT, class traits, class BidiIterator, class Allocator>
std::basic_ostream<charT, traits>&
   operator << (std::basic_ostream<charT, traits>& os,
                const match_results<BidiIterator, Allocator>& s)
{
   return (os << s.str());
}
#else
template <class BidiIterator, class Allocator>
std::ostream& operator << (std::ostream& os,
                           const match_results<BidiIterator, Allocator>& s)
{
   return (os << s.str());
}
#endif

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
} // namespace boost

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif


