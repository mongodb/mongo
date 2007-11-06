//=======================================================================
// Copyright 2001 Jeremy G. Siek
// Authors: Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

/*
 * Copyright (c) 1998
 * Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Silicon Graphics makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 */

#include <boost/config.hpp>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <boost/config.hpp>
#include <boost/pending/ct_if.hpp>
#include <boost/graph/detail/bitset_adaptor.hpp>

// This provides versions of std::bitset with both static and dynamic size.

// UNDER CONSTRUCTION


// replace this later
#include <cassert>
#define BOOST_ASSERT_THROW(expr, except) assert(expr)

namespace boost {

  namespace detail {
    // structure to aid in counting bits
    template<bool dummy = true>
    struct bit_count {
      static unsigned char value[256];
    };

    // Mapping from 8 bit unsigned integers to the index of the first bit
    template<bool dummy = true>
    struct first_bit_location {
      static unsigned char value[256];
    };

    template <typename WordType>  // this size is in bits
    struct word_traits {
      typedef WordType word_type;
      static const std::size_t word_size = CHAR_BIT * sizeof(word_type);
    };
    
    //=========================================================================
    template <class WordTraits, class SizeType, class Derived>
    class bitset_base
      : public bitset_adaptor< SizeType, 
                               bitset_base<WordTraits, SizeType, Derived> >
    {
      //    private:
    public:
      typedef SizeType size_type;
      typedef typename WordTraits::word_type word_type;

      static size_type s_which_word(size_type pos) {
        return pos / WordTraits::word_size;
      }
      static size_type s_which_byte(size_type pos) {
        return (pos % WordTraits::word_size) / CHAR_BIT;
      }
      static size_type s_which_bit(size_type pos) {
        return pos % WordTraits::word_size;
      }
      static word_type s_mask_bit(size_type pos) {
        return (static_cast<word_type>(1)) << s_which_bit(pos); 
      }
      word_type& m_get_word(size_type pos) {
        return data()[s_which_word(pos)]; 
      }
      word_type m_get_word(size_type pos) const {
        return data()[s_which_word(pos)]; 
      }
      word_type& m_hi_word() { return data()[num_words() - 1]; }
      word_type  m_hi_word() const { return data()[num_words() - 1]; }

      void m_sanitize_highest() {
        size_type extra_bits = size() % WordTraits::word_size;
        if (extra_bits)
          m_hi_word() &= ~((~static_cast<word_type>(0)) << extra_bits);
      }
    public:

      class reference {
        friend class bitset_base;

        word_type *m_word_ptr;
        size_type m_bit_pos;

        // left undefined
        reference();

        reference(bitset_base& b, size_type pos ) {
          m_word_ptr = &b.m_get_word(pos);
          m_bit_pos = s_which_bit(pos);
        }

      public:
        ~reference() {}

        // for b[i] = x;
        reference& operator=(bool x) {
          if ( x )
            *m_word_ptr |= s_mask_bit(m_bit_pos);
          else
            *m_word_ptr &= ~s_mask_bit(m_bit_pos);

          return *this;
        }
        // for b[i] = b[j];
        reference& operator=(const reference& j) {
          if ( (*(j.m_word_ptr) & s_mask_bit(j.m_bit_pos)) )
            *m_word_ptr |= s_mask_bit(m_bit_pos);
          else
            *m_word_ptr &= ~s_mask_bit(m_bit_pos);

          return *this;
        }
        // flips the bit
        bool operator~() const { 
          return (*(m_word_ptr) & s_mask_bit(m_bit_pos)) == 0; 
        }
        // for x = b[i];
        operator bool() const { 
          return (*(m_word_ptr) & s_mask_bit(m_bit_pos)) != 0; 
        }
        // for b[i].flip();
        reference& flip() {
          *m_word_ptr ^= s_mask_bit(m_bit_pos);
          return *this;
        }
      };

      void init_from_ulong(unsigned long val) {
        reset();
        const size_type n = (std::min)(sizeof(unsigned long) * CHAR_BIT,
                                     WordTraits::word_size * num_words());
        for(size_type i = 0; i < n; ++i, val >>= 1)
          if ( val & 0x1 )
            m_get_word(i) |= s_mask_bit(i);
      }
      
      // intersection: this = this & x
      Derived& operator&=(const Derived& x) {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] &= x.data()[i];
        return static_cast<Derived&>(*this);
      }
      // union: this = this | x
      Derived& operator|=(const Derived& x) {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] |= x.data()[i];
        return static_cast<Derived&>(*this);
      }
      // exclusive or: this = this ^ x
      Derived& operator^=(const Derived& x) {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] ^= x.data()[i];
        return static_cast<Derived&>(*this);
      }
      // left shift
      Derived& operator<<=(size_type pos);

      // right shift
      Derived& operator>>=(size_type pos);

      Derived& set() {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] = ~static_cast<word_type>(0);
        m_sanitize_highest();
        return static_cast<Derived&>(*this);
      }

      Derived& set(size_type pos, int val = true)
      {
        BOOST_ASSERT_THROW(pos < size(), std::out_of_range("boost::bitset::set(pos,value)"));
        if (val)
          m_get_word(pos) |= s_mask_bit(pos);
        else
          m_get_word(pos) &= ~s_mask_bit(pos);
        return static_cast<Derived&>(*this);
      }
      
      Derived& reset() {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] = 0;
        return static_cast<Derived&>(*this);
      }

      Derived& reset(size_type pos) {
        BOOST_ASSERT_THROW(pos < size(), std::out_of_range("boost::bitset::reset(pos)"));
        m_get_word(pos) &= ~s_mask_bit(pos);
        return static_cast<Derived&>(*this);
      }

      // compliment
      Derived operator~() const {
        return Derived(static_cast<const Derived&>(*this)).flip();
      }
      
      Derived& flip() {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] = ~data()[i];
        m_sanitize_highest();
        return static_cast<Derived&>(*this);
      }
      Derived& flip(size_type pos) {
        BOOST_ASSERT_THROW(pos < size(), std::out_of_range("boost::bitset::flip(pos)"));
        m_get_word(pos) ^= s_mask_bit(pos);
        return static_cast<Derived&>(*this);
      }

      // element access
      reference operator[](size_type pos) { return reference(*this, pos); }
      bool operator[](size_type pos) const { return test(pos); }

      unsigned long to_ulong() const;

      // to_string

      
      size_type count() const {
        size_type result = 0;
        const unsigned char* byte_ptr = (const unsigned char*)data();
        const unsigned char* end_ptr = 
          (const unsigned char*)(data() + num_words());
        while ( byte_ptr < end_ptr ) {
          result += bit_count<>::value[*byte_ptr];
          byte_ptr++;
        }
        return result;
      }   
      
      // size() must be provided by Derived class

      bool operator==(const Derived& x) const {
        return std::equal(data(), data() + num_words(), x.data());
      }

      bool operator!=(const Derived& x) const {
        return ! this->operator==(x);
      }

      bool test(size_type pos) const {
        BOOST_ASSERT_THROW(pos < size(), std::out_of_range("boost::bitset::test(pos)"));
        return (m_get_word(pos) & s_mask_bit(pos))
          != static_cast<word_type>(0);
      }

      bool any() const {
        for (size_type i = 0; i < num_words(); ++i) {
          if ( data()[i] != static_cast<word_type>(0) )
            return true;
        }
        return false;
      }
      bool none() const {
        return !any();
      }

      Derived operator<<(size_type pos) const
        { return Derived(static_cast<const Derived&>(*this)) <<= pos; }

      Derived operator>>(size_type pos) const
        { return Derived(static_cast<const Derived&>(*this)) >>= pos; }

      template <class CharT, class Traits, class Alloc>
      void m_copy_from_string(const basic_string<CharT,Traits,Alloc>& s,
                              size_type pos, size_type n)
      {
        reset();
        const size_type nbits = (std::min)(size(), (std::min)(n, s.size() - pos));
        for (size_type i = 0; i < nbits; ++i) {
          switch(s[pos + nbits - i - 1]) {
          case '0':
            break;
          case '1':
            this->set(i);
            break;
          default:
            throw std::invalid_argument
              ("boost::bitset_base::m_copy_from_string(s, pos, n)");
          }
        }
      }

      template <class CharT, class Traits, class Alloc>
      void m_copy_to_string(basic_string<CharT, Traits, Alloc>& s) const
      {
        s.assign(size(), '0');
        
        for (size_type i = 0; i < size(); ++i)
          if (test(i))
            s[size() - 1 - i] = '1';
      }

      //-----------------------------------------------------------------------
      // Stuff not in std::bitset

      // difference:  this = this - x
      Derived& operator-=(const Derived& x) {
        for (size_type i = 0; i < num_words(); ++i)
          data()[i] &= ~x.data()[i];
        return static_cast<Derived&>(*this);
      }

      // this wasn't working, why?
      int compare_3way(const Derived& x) const {
        return std::lexicographical_compare_3way
          (data(), data() + num_words(), x.data(), x.data() + x.num_words());
      }

      // less-than compare
      bool operator<(const Derived& x) const {
        return std::lexicographical_compare
          (data(), data() + num_words(), x.data(), x.data() + x.num_words());
      }

      // find the index of the first "on" bit
      size_type find_first() const;

      // find the index of the next "on" bit after prev
      size_type find_next(size_type prev) const;

      
      size_type _Find_first() const { return find_first(); }

      // find the index of the next "on" bit after prev
      size_type _Find_next(size_type prev) const { return find_next(prev); }

      //    private:
      word_type* data()
        { return static_cast<Derived*>(this)->data(); }

      const word_type* data() const 
        { return static_cast<const Derived*>(this)->data(); }

      size_type num_words() const 
        { return static_cast<const Derived*>(this)->num_words(); }

      size_type size() const 
        { return static_cast<const Derived*>(this)->size(); }
    };

    // 23.3.5.3 bitset operations:
    template <class W, class S, class D>
    inline D operator&(const bitset_base<W,S,D>& x,
                       const bitset_base<W,S,D>& y) {
      D result(static_cast<const D&>(x));
      result &= static_cast<const D&>(y);
      return result;
    }

    template <class W, class S, class D>
    inline D operator|(const bitset_base<W,S,D>& x,
                       const bitset_base<W,S,D>& y) {
      D result(static_cast<const D&>(x));
      result |= static_cast<const D&>(y);
      return result;
    }

    template <class W, class S, class D>
    inline D operator^(const bitset_base<W,S,D>& x,
                       const bitset_base<W,S,D>& y) {
      D result(static_cast<const D&>(x));
      result ^= static_cast<const D&>(y);
      return result;
    }

    // this one is an extension
    template <class W, class S, class D>
    inline D operator-(const bitset_base<W,S,D>& x,
                       const bitset_base<W,S,D>& y) {
      D result(static_cast<const D&>(x));
      result -= static_cast<const D&>(y);
      return result;
    }

    template <class W, class S, class D>
    inline int compare_3way(const bitset_base<W,S,D>& x,
                            const bitset_base<W,S,D>& y) {
      return std::lexicographical_compare_3way
        (x.data(), x.data() + x.num_words(), 
         y.data(), y.data() + y.num_words());
    }


    template <class W, class S, class D>
    std::istream&
    operator>>(std::istream& is, bitset_base<W,S,D>& x) {
      std::string tmp;
      tmp.reserve(x.size());

      // In new templatized iostreams, use istream::sentry
      if (is.flags() & ios::skipws) {
        char c;
        do
          is.get(c);
        while (is && isspace(c));
        if (is)
          is.putback(c);
      }

      for (S i = 0; i < x.size(); ++i) {
        char c;
        is.get(c);

        if (!is)
          break;
        else if (c != '0' && c != '1') {
          is.putback(c);
          break;
        }
        else
          //      tmp.push_back(c);
          tmp += c;
      }

      if (tmp.empty())
        is.clear(is.rdstate() | ios::failbit);
      else
        x.m_copy_from_string(tmp, static_cast<S>(0), x.size());

      return is;
    }

    template <class W, class S, class D>
    std::ostream& operator<<(std::ostream& os, 
                             const bitset_base<W,S,D>& x) {
      std::string tmp;
      x.m_copy_to_string(tmp);
      return os << tmp;
    }

    //=========================================================================
    template <typename WordType = unsigned long,
              typename SizeType = std::size_t,
              typename Allocator = std::allocator<WordType>
             >
    class dyn_size_bitset
      : public bitset_base<word_traits<WordType>, SizeType,
          dyn_size_bitset<WordType,SizeType,Allocator> >
    {
      typedef dyn_size_bitset self;
    public:
      typedef SizeType size_type;
    private:
      typedef word_traits<WordType> WordTraits;
      static const size_type word_size = WordTraits::word_size;

    public:
      dyn_size_bitset(unsigned long val, 
                      size_type n,
                      const Allocator& alloc = Allocator()) 
        : m_data(alloc.allocate((n + word_size - 1) / word_size)),
          m_size(n),
          m_num_words((n + word_size - 1) / word_size),
          m_alloc(alloc)
      {
        init_from_ulong(val);
      }

      dyn_size_bitset(size_type n,  // size of the set's "universe"
                      const Allocator& alloc = Allocator())
        : m_data(alloc.allocate((n + word_size - 1) / word_size)), 
          m_size(n), m_num_words((n + word_size - 1) / word_size),
          m_alloc(alloc)
      { }

      template<class CharT, class Traits, class Alloc>
      explicit dyn_size_bitset
        (const basic_string<CharT,Traits,Alloc>& s,
         std::size_t pos = 0,
         std::size_t n = std::size_t(basic_string<CharT,Traits,Alloc>::npos),
         const Allocator& alloc = Allocator())
        : m_data(alloc.allocate((n + word_size - 1) / word_size)), 
          m_size(n), m_num_words((n + word_size - 1) / word_size),
          m_alloc(alloc)
      {
        BOOST_ASSERT_THROW(pos < s.size(), std::out_of_range("dyn_size_bitset::dyn_size_bitset(s,pos,n,alloc)"));
        m_copy_from_string(s, pos, n);
      }

      template <typename InputIterator>
      explicit dyn_size_bitset
        (InputIterator first, InputIterator last,
         size_type n,  // size of the set's "universe"
         const Allocator& alloc = Allocator())
        : m_data(alloc.allocate((n + word_size - 1) / word_size)), 
          m_size(N), m_num_words((n + word_size - 1) / word_size),
          m_alloc(alloc)
      {
        while (first != last)
          this->set(*first++);
      }

      ~dyn_size_bitset() { 
        m_alloc.deallocate(m_data, m_num_words); 
      }
      
      size_type size() const { return m_size; }

      // protected:
      size_type num_words() const { return m_num_words; }

      word_type* data() { return m_data; }
      const word_type* data() const { return m_data; }

    protected:
      word_type* m_data;
      SizeType m_size;
      SizeType m_num_words;
      Allocator m_alloc;
    };

    //=========================================================================
    template <std::size_t N, typename WordType = unsigned long,
      typename SizeType = std::size_t>
    class bitset
      : public bitset_base<word_traits<WordType>, SizeType,
          bitset<N, WordType, SizeType> >
    {
      typedef bitset self;
      static const std::size_t word_size = word_traits<WordType>::word_size;
    public:
        // 23.3.5.1 constructors:
      bitset() {
#if defined(__GNUC__)
        for (size_type i = 0; i < num_words(); ++i)
          m_data[i] = static_cast<WordType>(0);
#endif
      }

      bitset(unsigned long val) {
        init_from_ulong(val);
      }

      template<class CharT, class Traits, class Alloc>
      explicit bitset
        (const basic_string<CharT,Traits,Alloc>& s,
         std::size_t pos = 0,
         std::size_t n = std::size_t(basic_string<CharT,Traits,Alloc>::npos))
      {
        BOOST_ASSERT_THROW
          (pos < s.size(), std::out_of_range("bitset::bitset(s,pos,n)"));
        m_copy_from_string(s, pos, n);
      }

      size_type size() const { return N; }

      // protected:
      size_type num_words() const { return (N + word_size - 1) / word_size; }

      word_type* data() { return m_data; }
      const word_type* data() const { return m_data; }
    protected:
      word_type m_data[(N + word_size - 1) / word_size];
    };

    //=========================================================================
    struct select_static_bitset {
      template <std::size_t N, typename WordT, typename SizeT, typename Alloc>
      struct bind_ {
        typedef bitset<N, WordT, SizeT> type;
      };
    };
    struct select_dyn_size_bitset {
      template <std::size_t N, typename WordT, typename SizeT, typename Alloc>
      struct bind_ {
        typedef dyn_size_bitset<WordT, SizeT, Alloc> type;
      };
    };

    template <std::size_t N = 0, // 0 means use dynamic
      typename WordType = unsigned long,
      typename Size_type = std::size_t, 
      typename Allocator = std::allocator<WordType>
             >
    class bitset_generator {
      typedef typename ct_if<N, select_dyn_size_bitset,
        select_static_bitset>::type selector;
    public:
      typedef typename selector
        ::template bind_<N, WordType, SizeType, Allocator>::type type;
    };


    //=========================================================================
    // bitset_base non-inline member function implementations

    template <class WordTraits, class SizeType, class Derived>
    Derived&
    bitset_base<WordTraits, SizeType, Derived>::
    operator<<=(size_type shift)
    {
      typedef typename WordTraits::word_type word_type;
      typedef SizeType size_type;
      if (shift != 0) {
        const size_type wshift = shift / WordTraits::word_size;
        const size_type offset = shift % WordTraits::word_size;
        const size_type sub_offset = WordTraits::word_size - offset;
        size_type n = num_words() - 1;
        for ( ; n > wshift; --n)
          data()[n] = (data()[n - wshift] << offset) |
            (data()[n - wshift - 1] >> sub_offset);
        if (n == wshift)
          data()[n] = data()[0] << offset;
        for (size_type n1 = 0; n1 < n; ++n1)
          data()[n1] = static_cast<word_type>(0);
      }
      m_sanitize_highest();
      return static_cast<Derived&>(*this);
    } // end operator<<=


    template <class WordTraits, class SizeType, class Derived>
    Derived&
    bitset_base<WordTraits, SizeType, Derived>::
    operator>>=(size_type shift)
    {
      typedef typename WordTraits::word_type word_type;
      typedef SizeType size_type;
      if (shift != 0) {
        const size_type wshift = shift / WordTraits::word_size;
        const size_type offset = shift % WordTraits::word_size;
        const size_type sub_offset = WordTraits::word_size - offset;
        const size_type limit = num_words() - wshift - 1;
        size_type n = 0;
        for ( ; n < limit; ++n)
          data()[n] = (data()[n + wshift] >> offset) |
            (data()[n + wshift + 1] << sub_offset);
        data()[limit] = data()[num_words()-1] >> offset;
        for (size_type n1 = limit + 1; n1 < num_words(); ++n1)
          data()[n1] = static_cast<word_type>(0);
      }
      m_sanitize_highest();
      return static_cast<Derived&>(*this);
    } // end operator>>=


    template <class WordTraits, class SizeType, class Derived>
    unsigned long bitset_base<WordTraits, SizeType, Derived>::
    to_ulong() const 
    {
      typedef typename WordTraits::word_type word_type;
      typedef SizeType size_type;
      const std::overflow_error
        overflow("boost::bit_set::operator unsigned long()");

      if (sizeof(word_type) >= sizeof(unsigned long)) {
        for (size_type i = 1; i < num_words(); ++i)
          BOOST_ASSERT_THROW(! data()[i], overflow);
        
        const word_type mask 
          = static_cast<word_type>(static_cast<unsigned long>(-1));
        BOOST_ASSERT_THROW(! (data()[0] & ~mask), overflow);
        
        return static_cast<unsigned long>(data()[0] & mask);
      }
      else { // sizeof(word_type) < sizeof(unsigned long).
        const size_type nwords =
          (sizeof(unsigned long) + sizeof(word_type) - 1) / sizeof(word_type);

        size_type min_nwords = nwords;
        if (num_words() > nwords) {
          for (size_type i = nwords; i < num_words(); ++i)
            BOOST_ASSERT_THROW(!data()[i], overflow);
        }
        else
          min_nwords = num_words();

        // If unsigned long is 8 bytes and word_type is 6 bytes, then
        // an unsigned long consists of all of one word plus 2 bytes
        // from another word.
        const size_type part = sizeof(unsigned long) % sizeof(word_type);

#if 0
        // bug in here?
        // >> to far?
        BOOST_ASSERT_THROW((part != 0 
                            && nwords <= num_words() 
                            && (data()[min_nwords - 1] >>
                                ((sizeof(word_type) - part) * CHAR_BIT)) != 0),
                           overflow);
#endif

        unsigned long result = 0;
        for (size_type i = 0; i < min_nwords; ++i) {
          result |= static_cast<unsigned long>(
             data()[i]) << (i * sizeof(word_type) * CHAR_BIT);
        }
        return result;
      }
    }// end operator unsigned long()


    template <class WordTraits, class SizeType, class Derived>
    SizeType bitset_base<WordTraits,SizeType,Derived>::
    find_first() const
    {
      SizeType not_found = size();
      for (size_type i = 0; i < num_words(); i++ ) {
        word_type thisword = data()[i];
        if ( thisword != static_cast<word_type>(0) ) {
          // find byte within word
          for ( std::size_t j = 0; j < sizeof(word_type); j++ ) {
            unsigned char this_byte
              = static_cast<unsigned char>(thisword & (~(unsigned char)0));
            if ( this_byte )
              return i * WordTraits::word_size + j * CHAR_BIT +
                first_bit_location<>::value[this_byte];

            thisword >>= CHAR_BIT;
          }
        }
      }
      // not found, so return an indication of failure.
      return not_found;
    }

    template <class WordTraits, class SizeType, class Derived>
    SizeType bitset_base<WordTraits, SizeType, Derived>::
    bitset_base<WordTraits,SizeType,Derived>::
    find_next(size_type prev) const
    {
      SizeType not_found = size();
      // make bound inclusive
      ++prev;

      // check out of bounds
      if ( prev >= num_words() * WordTraits::word_size )
        return not_found;

        // search first word
      size_type i = s_which_word(prev);
      word_type thisword = data()[i];

        // mask off bits below bound
      thisword &= (~static_cast<word_type>(0)) << s_which_bit(prev);

      if ( thisword != static_cast<word_type>(0) ) {
        // find byte within word
        // get first byte into place
        thisword >>= s_which_byte(prev) * CHAR_BIT;
        for ( size_type j = s_which_byte(prev); j < sizeof(word_type); j++ ) {
          unsigned char this_byte
            = static_cast<unsigned char>(thisword & (~(unsigned char)0));
          if ( this_byte )
            return i * WordTraits::word_size + j * CHAR_BIT +
              first_bit_location<>::value[this_byte];

          thisword >>= CHAR_BIT;
        }
      }

      // check subsequent words
      i++;
      for ( ; i < num_words(); i++ ) {
        word_type thisword = data()[i];
        if ( thisword != static_cast<word_type>(0) ) {
          // find byte within word
          for ( size_type j = 0; j < sizeof(word_type); j++ ) {
            unsigned char this_byte
              = static_cast<unsigned char>(thisword & (~(unsigned char)0));
            if ( this_byte )
              return i * WordTraits::word_size + j * CHAR_BIT +
                first_bit_location<>::value[this_byte];

            thisword >>= CHAR_BIT;
          }
        }
      }

      // not found, so return an indication of failure.
      return not_found;
    } // end find_next


    template <bool dummy>
    unsigned char bit_count<dummy>::value[] = {
      0, /*   0 */ 1, /*   1 */ 1, /*   2 */ 2, /*   3 */ 1, /*   4 */
      2, /*   5 */ 2, /*   6 */ 3, /*   7 */ 1, /*   8 */ 2, /*   9 */
      2, /*  10 */ 3, /*  11 */ 2, /*  12 */ 3, /*  13 */ 3, /*  14 */
      4, /*  15 */ 1, /*  16 */ 2, /*  17 */ 2, /*  18 */ 3, /*  19 */
      2, /*  20 */ 3, /*  21 */ 3, /*  22 */ 4, /*  23 */ 2, /*  24 */
      3, /*  25 */ 3, /*  26 */ 4, /*  27 */ 3, /*  28 */ 4, /*  29 */
      4, /*  30 */ 5, /*  31 */ 1, /*  32 */ 2, /*  33 */ 2, /*  34 */
      3, /*  35 */ 2, /*  36 */ 3, /*  37 */ 3, /*  38 */ 4, /*  39 */
      2, /*  40 */ 3, /*  41 */ 3, /*  42 */ 4, /*  43 */ 3, /*  44 */
      4, /*  45 */ 4, /*  46 */ 5, /*  47 */ 2, /*  48 */ 3, /*  49 */
      3, /*  50 */ 4, /*  51 */ 3, /*  52 */ 4, /*  53 */ 4, /*  54 */
      5, /*  55 */ 3, /*  56 */ 4, /*  57 */ 4, /*  58 */ 5, /*  59 */
      4, /*  60 */ 5, /*  61 */ 5, /*  62 */ 6, /*  63 */ 1, /*  64 */
      2, /*  65 */ 2, /*  66 */ 3, /*  67 */ 2, /*  68 */ 3, /*  69 */
      3, /*  70 */ 4, /*  71 */ 2, /*  72 */ 3, /*  73 */ 3, /*  74 */
      4, /*  75 */ 3, /*  76 */ 4, /*  77 */ 4, /*  78 */ 5, /*  79 */
      2, /*  80 */ 3, /*  81 */ 3, /*  82 */ 4, /*  83 */ 3, /*  84 */
      4, /*  85 */ 4, /*  86 */ 5, /*  87 */ 3, /*  88 */ 4, /*  89 */
      4, /*  90 */ 5, /*  91 */ 4, /*  92 */ 5, /*  93 */ 5, /*  94 */
      6, /*  95 */ 2, /*  96 */ 3, /*  97 */ 3, /*  98 */ 4, /*  99 */
      3, /* 100 */ 4, /* 101 */ 4, /* 102 */ 5, /* 103 */ 3, /* 104 */
      4, /* 105 */ 4, /* 106 */ 5, /* 107 */ 4, /* 108 */ 5, /* 109 */
      5, /* 110 */ 6, /* 111 */ 3, /* 112 */ 4, /* 113 */ 4, /* 114 */
      5, /* 115 */ 4, /* 116 */ 5, /* 117 */ 5, /* 118 */ 6, /* 119 */
      4, /* 120 */ 5, /* 121 */ 5, /* 122 */ 6, /* 123 */ 5, /* 124 */
      6, /* 125 */ 6, /* 126 */ 7, /* 127 */ 1, /* 128 */ 2, /* 129 */
      2, /* 130 */ 3, /* 131 */ 2, /* 132 */ 3, /* 133 */ 3, /* 134 */
      4, /* 135 */ 2, /* 136 */ 3, /* 137 */ 3, /* 138 */ 4, /* 139 */
      3, /* 140 */ 4, /* 141 */ 4, /* 142 */ 5, /* 143 */ 2, /* 144 */
      3, /* 145 */ 3, /* 146 */ 4, /* 147 */ 3, /* 148 */ 4, /* 149 */
      4, /* 150 */ 5, /* 151 */ 3, /* 152 */ 4, /* 153 */ 4, /* 154 */
      5, /* 155 */ 4, /* 156 */ 5, /* 157 */ 5, /* 158 */ 6, /* 159 */
      2, /* 160 */ 3, /* 161 */ 3, /* 162 */ 4, /* 163 */ 3, /* 164 */
      4, /* 165 */ 4, /* 166 */ 5, /* 167 */ 3, /* 168 */ 4, /* 169 */
      4, /* 170 */ 5, /* 171 */ 4, /* 172 */ 5, /* 173 */ 5, /* 174 */
      6, /* 175 */ 3, /* 176 */ 4, /* 177 */ 4, /* 178 */ 5, /* 179 */
      4, /* 180 */ 5, /* 181 */ 5, /* 182 */ 6, /* 183 */ 4, /* 184 */
      5, /* 185 */ 5, /* 186 */ 6, /* 187 */ 5, /* 188 */ 6, /* 189 */
      6, /* 190 */ 7, /* 191 */ 2, /* 192 */ 3, /* 193 */ 3, /* 194 */
      4, /* 195 */ 3, /* 196 */ 4, /* 197 */ 4, /* 198 */ 5, /* 199 */
      3, /* 200 */ 4, /* 201 */ 4, /* 202 */ 5, /* 203 */ 4, /* 204 */
      5, /* 205 */ 5, /* 206 */ 6, /* 207 */ 3, /* 208 */ 4, /* 209 */
      4, /* 210 */ 5, /* 211 */ 4, /* 212 */ 5, /* 213 */ 5, /* 214 */
      6, /* 215 */ 4, /* 216 */ 5, /* 217 */ 5, /* 218 */ 6, /* 219 */
      5, /* 220 */ 6, /* 221 */ 6, /* 222 */ 7, /* 223 */ 3, /* 224 */
      4, /* 225 */ 4, /* 226 */ 5, /* 227 */ 4, /* 228 */ 5, /* 229 */
      5, /* 230 */ 6, /* 231 */ 4, /* 232 */ 5, /* 233 */ 5, /* 234 */
      6, /* 235 */ 5, /* 236 */ 6, /* 237 */ 6, /* 238 */ 7, /* 239 */
      4, /* 240 */ 5, /* 241 */ 5, /* 242 */ 6, /* 243 */ 5, /* 244 */
      6, /* 245 */ 6, /* 246 */ 7, /* 247 */ 5, /* 248 */ 6, /* 249 */
      6, /* 250 */ 7, /* 251 */ 6, /* 252 */ 7, /* 253 */ 7, /* 254 */
      8  /* 255 */
    }; // end _Bit_count

    template <bool dummy>
    unsigned char first_bit_location<dummy>::value[] = {
      0, /*   0 */ 0, /*   1 */ 1, /*   2 */ 0, /*   3 */ 2, /*   4 */
      0, /*   5 */ 1, /*   6 */ 0, /*   7 */ 3, /*   8 */ 0, /*   9 */
      1, /*  10 */ 0, /*  11 */ 2, /*  12 */ 0, /*  13 */ 1, /*  14 */
      0, /*  15 */ 4, /*  16 */ 0, /*  17 */ 1, /*  18 */ 0, /*  19 */
      2, /*  20 */ 0, /*  21 */ 1, /*  22 */ 0, /*  23 */ 3, /*  24 */
      0, /*  25 */ 1, /*  26 */ 0, /*  27 */ 2, /*  28 */ 0, /*  29 */
      1, /*  30 */ 0, /*  31 */ 5, /*  32 */ 0, /*  33 */ 1, /*  34 */
      0, /*  35 */ 2, /*  36 */ 0, /*  37 */ 1, /*  38 */ 0, /*  39 */
      3, /*  40 */ 0, /*  41 */ 1, /*  42 */ 0, /*  43 */ 2, /*  44 */
      0, /*  45 */ 1, /*  46 */ 0, /*  47 */ 4, /*  48 */ 0, /*  49 */
      1, /*  50 */ 0, /*  51 */ 2, /*  52 */ 0, /*  53 */ 1, /*  54 */
      0, /*  55 */ 3, /*  56 */ 0, /*  57 */ 1, /*  58 */ 0, /*  59 */
      2, /*  60 */ 0, /*  61 */ 1, /*  62 */ 0, /*  63 */ 6, /*  64 */
      0, /*  65 */ 1, /*  66 */ 0, /*  67 */ 2, /*  68 */ 0, /*  69 */
      1, /*  70 */ 0, /*  71 */ 3, /*  72 */ 0, /*  73 */ 1, /*  74 */
      0, /*  75 */ 2, /*  76 */ 0, /*  77 */ 1, /*  78 */ 0, /*  79 */
      4, /*  80 */ 0, /*  81 */ 1, /*  82 */ 0, /*  83 */ 2, /*  84 */
      0, /*  85 */ 1, /*  86 */ 0, /*  87 */ 3, /*  88 */ 0, /*  89 */
      1, /*  90 */ 0, /*  91 */ 2, /*  92 */ 0, /*  93 */ 1, /*  94 */
      0, /*  95 */ 5, /*  96 */ 0, /*  97 */ 1, /*  98 */ 0, /*  99 */
      2, /* 100 */ 0, /* 101 */ 1, /* 102 */ 0, /* 103 */ 3, /* 104 */
      0, /* 105 */ 1, /* 106 */ 0, /* 107 */ 2, /* 108 */ 0, /* 109 */
      1, /* 110 */ 0, /* 111 */ 4, /* 112 */ 0, /* 113 */ 1, /* 114 */
      0, /* 115 */ 2, /* 116 */ 0, /* 117 */ 1, /* 118 */ 0, /* 119 */
      3, /* 120 */ 0, /* 121 */ 1, /* 122 */ 0, /* 123 */ 2, /* 124 */
      0, /* 125 */ 1, /* 126 */ 0, /* 127 */ 7, /* 128 */ 0, /* 129 */
      1, /* 130 */ 0, /* 131 */ 2, /* 132 */ 0, /* 133 */ 1, /* 134 */
      0, /* 135 */ 3, /* 136 */ 0, /* 137 */ 1, /* 138 */ 0, /* 139 */
      2, /* 140 */ 0, /* 141 */ 1, /* 142 */ 0, /* 143 */ 4, /* 144 */
      0, /* 145 */ 1, /* 146 */ 0, /* 147 */ 2, /* 148 */ 0, /* 149 */
      1, /* 150 */ 0, /* 151 */ 3, /* 152 */ 0, /* 153 */ 1, /* 154 */
      0, /* 155 */ 2, /* 156 */ 0, /* 157 */ 1, /* 158 */ 0, /* 159 */
      5, /* 160 */ 0, /* 161 */ 1, /* 162 */ 0, /* 163 */ 2, /* 164 */
      0, /* 165 */ 1, /* 166 */ 0, /* 167 */ 3, /* 168 */ 0, /* 169 */
      1, /* 170 */ 0, /* 171 */ 2, /* 172 */ 0, /* 173 */ 1, /* 174 */
      0, /* 175 */ 4, /* 176 */ 0, /* 177 */ 1, /* 178 */ 0, /* 179 */
      2, /* 180 */ 0, /* 181 */ 1, /* 182 */ 0, /* 183 */ 3, /* 184 */
      0, /* 185 */ 1, /* 186 */ 0, /* 187 */ 2, /* 188 */ 0, /* 189 */
      1, /* 190 */ 0, /* 191 */ 6, /* 192 */ 0, /* 193 */ 1, /* 194 */
      0, /* 195 */ 2, /* 196 */ 0, /* 197 */ 1, /* 198 */ 0, /* 199 */
      3, /* 200 */ 0, /* 201 */ 1, /* 202 */ 0, /* 203 */ 2, /* 204 */
      0, /* 205 */ 1, /* 206 */ 0, /* 207 */ 4, /* 208 */ 0, /* 209 */
      1, /* 210 */ 0, /* 211 */ 2, /* 212 */ 0, /* 213 */ 1, /* 214 */
      0, /* 215 */ 3, /* 216 */ 0, /* 217 */ 1, /* 218 */ 0, /* 219 */
      2, /* 220 */ 0, /* 221 */ 1, /* 222 */ 0, /* 223 */ 5, /* 224 */
      0, /* 225 */ 1, /* 226 */ 0, /* 227 */ 2, /* 228 */ 0, /* 229 */
      1, /* 230 */ 0, /* 231 */ 3, /* 232 */ 0, /* 233 */ 1, /* 234 */
      0, /* 235 */ 2, /* 236 */ 0, /* 237 */ 1, /* 238 */ 0, /* 239 */
      4, /* 240 */ 0, /* 241 */ 1, /* 242 */ 0, /* 243 */ 2, /* 244 */
      0, /* 245 */ 1, /* 246 */ 0, /* 247 */ 3, /* 248 */ 0, /* 249 */
      1, /* 250 */ 0, /* 251 */ 2, /* 252 */ 0, /* 253 */ 1, /* 254 */
      0, /* 255 */
    }; // end _First_one

  } // namespace detail

} // namespace boost
