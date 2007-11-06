// --------------------------------------------------
//
// (C) Copyright Chuck Allison and Jeremy Siek 2001 - 2002.
// (C) Copyright Gennaro Prota                 2003 - 2004.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// -----------------------------------------------------------

//  See http://www.boost.org/libs/dynamic_bitset for documentation.



#ifndef BOOST_DYNAMIC_BITSET_DYNAMIC_BITSET_HPP
#define BOOST_DYNAMIC_BITSET_DYNAMIC_BITSET_HPP

#include <cassert>
#include <string>
#include <stdexcept>           // for std::overflow_error
#include <algorithm>           // for std::swap, std::min, std::copy, std::fill
#include <vector>
#include <climits>             // for CHAR_BIT

#include "boost/dynamic_bitset/config.hpp"

#ifndef BOOST_NO_STD_LOCALE
# include <locale> // G.P.S
#endif

#if defined(BOOST_OLD_IOSTREAMS)
#  include <iostream.h>
#  include <ctype.h> // for isspace
#else
#  include <istream>
#  include <ostream>
#endif

#include "boost/dynamic_bitset_fwd.hpp"
#include "boost/detail/dynamic_bitset.hpp"
#include "boost/detail/iterator.hpp" // used to implement append(Iter, Iter)
#include "boost/static_assert.hpp"
#include "boost/limits.hpp"
#include "boost/pending/lowest_bit.hpp" // used by find_first/next


namespace boost {

template

#if defined(BOOST_MSVC) && BOOST_WORKAROUND(BOOST_MSVC, <= 1300)  // 1300 == VC++ 7.0
   // VC++ (up to 7.0) wants the default arguments again
   <typename Block = unsigned long, typename Allocator = std::allocator<Block> >
# else
   <typename Block, typename Allocator>
# endif

class dynamic_bitset
{
  // Portability note: member function templates are defined inside
  // this class definition to avoid problems with VC++. Similarly,
  // with the member functions of nested classes.

  BOOST_STATIC_ASSERT(detail::dynamic_bitset_allowed_block_type<Block>::value);

public:
    typedef Block block_type;
    typedef Allocator allocator_type;
    typedef std::size_t size_type;
    typedef int block_width_type; // gps

    BOOST_STATIC_CONSTANT(block_width_type, bits_per_block = (std::numeric_limits<Block>::digits));
    BOOST_STATIC_CONSTANT(size_type, npos = static_cast<size_type>(-1));


public:

    // A proxy class to simulate lvalues of bit type.
    // Shouldn't it be private? [gps]
    //
    class reference
    {
        friend class dynamic_bitset<Block, Allocator>;


        // the one and only non-copy ctor
        reference(block_type & b, int pos)
            :m_block(b), m_mask(block_type(1) << pos)
        {}

        void operator&(); // left undefined

    public:

        // copy constructor: compiler generated

        operator bool() const { return (m_block & m_mask) != 0; }
        bool operator~() const { return (m_block & m_mask) == 0; }

        reference& flip() { do_flip(); return *this; }

        reference& operator=(bool x)               { do_assign(x);   return *this; } // for b[i] = x
        reference& operator=(const reference& rhs) { do_assign(rhs); return *this; } // for b[i] = b[j]

        reference& operator|=(bool x) { if  (x) do_set();   return *this; }
        reference& operator&=(bool x) { if (!x) do_reset(); return *this; }
        reference& operator^=(bool x) { if  (x) do_flip();  return *this; }
        reference& operator-=(bool x) { if  (x) do_reset(); return *this; }

     private:
        block_type & m_block;
        const block_type m_mask;

        void do_set() { m_block |= m_mask; }
        void do_reset() { m_block &= ~m_mask; }
        void do_flip() { m_block ^= m_mask; }
        void do_assign(bool x) { x? do_set() : do_reset(); }
    };

    typedef bool const_reference;

    // constructors, etc.
    explicit
    dynamic_bitset(const Allocator& alloc = Allocator());

    explicit
    dynamic_bitset(size_type num_bits, unsigned long value = 0,
               const Allocator& alloc = Allocator());


    // The presence of this constructor is a concession to ease of
    // use, especially for the novice user. A conversion from string
    // is, in most cases, formatting, and should be done by the standard
    // formatting convention: operator>>.
    //
    // NOTE:
    // Leave the parentheses around std::basic_string<CharT, Traits, Alloc>::npos.
    // g++ 3.2 requires them and probably the standard will - see core issue 325
    // NOTE 2: 
    // split into two constructors because of bugs in MSVC 6.0sp5 with STLport

    template <typename CharT, typename Traits, typename Alloc>
    dynamic_bitset(const std::basic_string<CharT, Traits, Alloc>& s,
        typename std::basic_string<CharT, Traits, Alloc>::size_type pos,
        typename std::basic_string<CharT, Traits, Alloc>::size_type n,
        size_type num_bits = npos,
        const Allocator& alloc = Allocator())

    :m_bits(alloc),
     m_num_bits(0)
    {
      init_from_string(s, pos, n, num_bits, alloc);
    }

    template <typename CharT, typename Traits, typename Alloc>
    explicit
    dynamic_bitset(const std::basic_string<CharT, Traits, Alloc>& s,
      typename std::basic_string<CharT, Traits, Alloc>::size_type pos = 0)

    :m_bits(Allocator()),
     m_num_bits(0)
    {
      init_from_string(s, pos, (std::basic_string<CharT, Traits, Alloc>::npos),
                       npos, Allocator());
    }

    // The first bit in *first is the least significant bit, and the
    // last bit in the block just before *last is the most significant bit.
    template <typename BlockInputIterator>
    dynamic_bitset(BlockInputIterator first, BlockInputIterator last,
                   const Allocator& alloc = Allocator())

    :m_bits(first, last, alloc),
     m_num_bits(m_bits.size() * bits_per_block)
    {}


    // copy constructor
    dynamic_bitset(const dynamic_bitset& b);

    ~dynamic_bitset();

    void swap(dynamic_bitset& b);
    dynamic_bitset& operator=(const dynamic_bitset& b);

    allocator_type get_allocator() const;

    // size changing operations
    void resize(size_type num_bits, bool value = false);
    void clear();
    void push_back(bool bit);
    void append(Block block);

    template <typename BlockInputIterator>
    void m_append(BlockInputIterator first, BlockInputIterator last, std::input_iterator_tag)
    {
        std::vector<Block, Allocator> v(first, last);
        m_append(v.begin(), v.end(), std::random_access_iterator_tag());
    }
    template <typename BlockInputIterator>
    void m_append(BlockInputIterator first, BlockInputIterator last, std::forward_iterator_tag)
    {
        assert(first != last);
        block_width_type r = count_extra_bits();
        std::size_t d = boost::detail::distance(first, last);
        m_bits.reserve(num_blocks() + d);
        if (r == 0) {
            for( ; first != last; ++first)
                m_bits.push_back(*first); // could use vector<>::insert()
        }
        else {
            m_highest_block() |= (*first << r);
            do {
                Block b = *first >> (bits_per_block - r);
                ++first;
                m_bits.push_back(b | (first==last? 0 : *first << r));
            } while (first != last);
        }
        m_num_bits += bits_per_block * d;
    }
    template <typename BlockInputIterator>
    void append(BlockInputIterator first, BlockInputIterator last) // strong guarantee
    {
        if (first != last) {
            typename detail::iterator_traits<BlockInputIterator>::iterator_category cat;
            m_append(first, last, cat);
        }
    }


    // bitset operations
    dynamic_bitset& operator&=(const dynamic_bitset& b);
    dynamic_bitset& operator|=(const dynamic_bitset& b);
    dynamic_bitset& operator^=(const dynamic_bitset& b);
    dynamic_bitset& operator-=(const dynamic_bitset& b);
    dynamic_bitset& operator<<=(size_type n);
    dynamic_bitset& operator>>=(size_type n);
    dynamic_bitset operator<<(size_type n) const;
    dynamic_bitset operator>>(size_type n) const;

    // basic bit operations
    dynamic_bitset& set(size_type n, bool val = true);
    dynamic_bitset& set();
    dynamic_bitset& reset(size_type n);
    dynamic_bitset& reset();
    dynamic_bitset& flip(size_type n);
    dynamic_bitset& flip();
    bool test(size_type n) const;
    bool any() const;
    bool none() const;
    dynamic_bitset operator~() const;
    size_type count() const;

    // subscript
    reference operator[](size_type pos) {
        return reference(m_bits[block_index(pos)], bit_index(pos));
    }
    bool operator[](size_type pos) const { return test(pos); }

    unsigned long to_ulong() const;

    size_type size() const;
    size_type num_blocks() const;
    size_type max_size() const;
    bool empty() const;
#if 0 // gps
    void reserve(size_type n);
    size_type capacity() const;
#endif

    bool is_subset_of(const dynamic_bitset& a) const;
    bool is_proper_subset_of(const dynamic_bitset& a) const;
    bool intersects(const dynamic_bitset & a) const;

    // lookup
    size_type find_first() const;
    size_type find_next(size_type pos) const;


#if !defined BOOST_DYNAMIC_BITSET_DONT_USE_FRIENDS
    // lexicographical comparison
    template <typename B, typename A>
    friend bool operator==(const dynamic_bitset<B, A>& a,
                           const dynamic_bitset<B, A>& b);

    template <typename B, typename A>
    friend bool operator<(const dynamic_bitset<B, A>& a,
                          const dynamic_bitset<B, A>& b);


    template <typename B, typename A, typename BlockOutputIterator>
    friend void to_block_range(const dynamic_bitset<B, A>& b,
                               BlockOutputIterator result);

    template <typename BlockIterator, typename B, typename A>
    friend void from_block_range(BlockIterator first, BlockIterator last,
                                 dynamic_bitset<B, A>& result);


    template <typename CharT, typename Traits, typename B, typename A>
    friend std::basic_istream<CharT, Traits>& operator>>(std::basic_istream<CharT, Traits>& is,
                                                         dynamic_bitset<B, A>& b);

    template <typename B, typename A, typename stringT>
    friend void to_string_helper(const dynamic_bitset<B, A> & b, stringT & s, bool dump_all);


#endif


private:
    BOOST_STATIC_CONSTANT(block_width_type, ulong_width = std::numeric_limits<unsigned long>::digits);
    typedef std::vector<block_type, allocator_type> buffer_type;

    void m_zero_unused_bits();
    bool m_check_invariants() const;

    size_type m_do_find_from(size_type first_block) const;

    block_width_type count_extra_bits() const { return bit_index(size()); }
    static size_type block_index(size_type pos) { return pos / bits_per_block; }
    static block_width_type bit_index(size_type pos) { return static_cast<int>(pos % bits_per_block); }
    static Block bit_mask(size_type pos) { return Block(1) << bit_index(pos); }

    template <typename CharT, typename Traits, typename Alloc>
    void init_from_string(const std::basic_string<CharT, Traits, Alloc>& s,
        typename std::basic_string<CharT, Traits, Alloc>::size_type pos,
        typename std::basic_string<CharT, Traits, Alloc>::size_type n,
        size_type num_bits,
        const Allocator& alloc)
    {
        assert(pos <= s.size());

        typedef typename std::basic_string<CharT, Traits, Alloc> StrT;
        typedef typename StrT::traits_type Tr;

        const typename StrT::size_type rlen = (std::min)(n, s.size() - pos); // gps
        const size_type sz = ( num_bits != npos? num_bits : rlen);
        m_bits.resize(calc_num_blocks(sz));
        m_num_bits = sz;


        BOOST_DYNAMIC_BITSET_CTYPE_FACET(CharT, fac, std::locale());
        const CharT one = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '1');

        const size_type m = num_bits < rlen ? num_bits : rlen; // [gps]
        typename StrT::size_type i = 0;
        for( ; i < m; ++i) {

            const CharT c = s[(pos + m - 1) - i];

            assert( Tr::eq(c, one)
                    || Tr::eq(c, BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '0')) );

            if (Tr::eq(c, one))
                set(i);

        }

    }

BOOST_DYNAMIC_BITSET_PRIVATE:

    bool m_unchecked_test(size_type pos) const;
    static size_type calc_num_blocks(size_type num_bits);

    Block&        m_highest_block();
    const Block&  m_highest_block() const;

    buffer_type m_bits; // [gps] to be renamed
    size_type   m_num_bits;


    class bit_appender;
    friend class bit_appender;
    class bit_appender {
      // helper for stream >>
      // Supplies to the lack of an efficient append at the less
      // significant end: bits are actually appended "at left" but
      // rearranged in the destructor. Everything works just as if
      // dynamic_bitset<> had an append_at_right() function (which
      // threw, in case, the same exceptions as push_back) except
      // that the function is actually called bit_appender::do_append().
      //
      dynamic_bitset & bs;
      size_type n;
      Block mask;
      Block * current;
    public:
        bit_appender(dynamic_bitset & r) : bs(r), n(0), mask(0), current(0) {}
        ~bit_appender() {
            // reverse the order of blocks, shift
            // if needed, and then resize
            //
            std::reverse(bs.m_bits.begin(), bs.m_bits.end());
            const block_width_type offs = bit_index(n);
            if (offs)
                bs >>= (bits_per_block - offs);
            bs.resize(n); // doesn't enlarge, so can't throw
            assert(bs.m_check_invariants());
        }
        inline void do_append(bool value) {

            if (mask == 0) {
                bs.append(Block(0));
                current = &bs.m_highest_block();
                mask = Block(1) << (bits_per_block - 1);
            }

            if(value)
                *current |= mask;

            mask /= 2;
            ++n;
        }
        size_type get_count() const { return n; }
    };

};

#if BOOST_WORKAROUND( __IBMCPP__, <=600 )

// Workaround for IBM's AIX platform.
// See http://comments.gmane.org/gmane.comp.lib.boost.user/15331

template<typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>::block_width_type const
dynamic_bitset<Block, Allocator>::bits_per_block;

template<typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>::block_width_type const
dynamic_bitset<Block, Allocator>::ulong_width;

#endif

// Global Functions:

// comparison
template <typename Block, typename Allocator>
bool operator!=(const dynamic_bitset<Block, Allocator>& a,
                const dynamic_bitset<Block, Allocator>& b);

template <typename Block, typename Allocator>
bool operator<=(const dynamic_bitset<Block, Allocator>& a,
                const dynamic_bitset<Block, Allocator>& b);

template <typename Block, typename Allocator>
bool operator>(const dynamic_bitset<Block, Allocator>& a,
               const dynamic_bitset<Block, Allocator>& b);

template <typename Block, typename Allocator>
bool operator>=(const dynamic_bitset<Block, Allocator>& a,
                const dynamic_bitset<Block, Allocator>& b);

// stream operators
#ifdef BOOST_OLD_IOSTREAMS
template <typename Block, typename Allocator>
std::ostream& operator<<(std::ostream& os,
                         const dynamic_bitset<Block, Allocator>& b);

template <typename Block, typename Allocator>
std::istream& operator>>(std::istream& is, dynamic_bitset<Block,Allocator>& b);
#else
// NOTE: Digital Mars wants the same template parameter names
//       here and in the definition! [last tested: 8.48.10]
//
template <typename Ch, typename Tr, typename Block, typename Alloc>
std::basic_ostream<Ch, Tr>&
operator<<(std::basic_ostream<Ch, Tr>& os,
           const dynamic_bitset<Block, Alloc>& b);

template <typename Ch, typename Tr, typename Block, typename Alloc>
std::basic_istream<Ch, Tr>&
operator>>(std::basic_istream<Ch, Tr>& is,
           dynamic_bitset<Block, Alloc>& b);
#endif

// bitset operations
template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator&(const dynamic_bitset<Block, Allocator>& b1,
          const dynamic_bitset<Block, Allocator>& b2);

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator|(const dynamic_bitset<Block, Allocator>& b1,
          const dynamic_bitset<Block, Allocator>& b2);

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator^(const dynamic_bitset<Block, Allocator>& b1,
          const dynamic_bitset<Block, Allocator>& b2);

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator-(const dynamic_bitset<Block, Allocator>& b1,
          const dynamic_bitset<Block, Allocator>& b2);

// namespace scope swap
template<typename Block, typename Allocator>
void swap(dynamic_bitset<Block, Allocator>& b1,
          dynamic_bitset<Block, Allocator>& b2);


template <typename Block, typename Allocator, typename stringT>
void
to_string(const dynamic_bitset<Block, Allocator>& b, stringT & s); // gps

template <typename Block, typename Allocator, typename BlockOutputIterator>
void
to_block_range(const dynamic_bitset<Block, Allocator>& b,
               BlockOutputIterator result);


// gps - check docs with Jeremy
//
template <typename BlockIterator, typename B, typename A>
inline void
from_block_range(BlockIterator first, BlockIterator last,
                 dynamic_bitset<B, A>& result)
{
    // PRE: distance(first, last) <= numblocks()
    std::copy (first, last, result.m_bits.begin()); //[gps]
}

//=============================================================================
// dynamic_bitset implementation


//-----------------------------------------------------------------------------
// constructors, etc.

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>::dynamic_bitset(const Allocator& alloc)
  : m_bits(alloc), m_num_bits(0)
{

}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>::
dynamic_bitset(size_type num_bits, unsigned long value, const Allocator& alloc)
  : m_bits(calc_num_blocks(num_bits), Block(0), alloc),
    m_num_bits(num_bits)
{

  if (num_bits == 0)
      return;

  typedef unsigned long num_type;

  // cut off all bits in value that have pos >= num_bits, if any
  if (num_bits < static_cast<size_type>(ulong_width)) {
      const num_type mask = (num_type(1) << num_bits) - 1;
      value &= mask;
  }

  if (bits_per_block >= ulong_width) {
      m_bits[0] = static_cast<block_type>(value);
  }
  else {
      for(size_type i = 0; value != 0; ++i) {

          m_bits[i] = static_cast<block_type>(value);
          value >>= BOOST_DYNAMIC_BITSET_WRAP_CONSTANT(bits_per_block);
      }
  }

}

// copy constructor
template <typename Block, typename Allocator>
inline dynamic_bitset<Block, Allocator>::
dynamic_bitset(const dynamic_bitset& b)
  : m_bits(b.m_bits), m_num_bits(b.m_num_bits)  // [gps]
{

}

template <typename Block, typename Allocator>
inline dynamic_bitset<Block, Allocator>::
~dynamic_bitset()
{
    assert(m_check_invariants());
}

template <typename Block, typename Allocator>
inline void dynamic_bitset<Block, Allocator>::
swap(dynamic_bitset<Block, Allocator>& b) // no throw
{
    std::swap(m_bits, b.m_bits);
    std::swap(m_num_bits, b.m_num_bits);
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>& dynamic_bitset<Block, Allocator>::
operator=(const dynamic_bitset<Block, Allocator>& b)
{
#if 0 // gps
    dynamic_bitset<Block, Allocator> tmp(b);
    this->swap(tmp);
    return *this;
#else
    m_bits = b.m_bits;
    m_num_bits = b.m_num_bits;
    return *this;
#endif
}

template <typename Block, typename Allocator>
inline typename dynamic_bitset<Block, Allocator>::allocator_type
dynamic_bitset<Block, Allocator>::get_allocator() const
{
    return m_bits.get_allocator();
}

//-----------------------------------------------------------------------------
// size changing operations

template <typename Block, typename Allocator>
void dynamic_bitset<Block, Allocator>::
resize(size_type num_bits, bool value) // strong guarantee
{

  const size_type old_num_blocks = num_blocks();
  const size_type required_blocks = calc_num_blocks(num_bits);

  const block_type v = value? ~Block(0) : Block(0);

  if (required_blocks != old_num_blocks) {
    m_bits.resize(required_blocks, v); // s.g. (copy) [gps]
  }


  // At this point:
  //
  //  - if the buffer was shrunk, there's nothing to do, except
  //    a call to m_zero_unused_bits()
  //
  //  - if it it is enlarged, all the (used) bits in the new blocks have
  //    the correct value, but we should also take care of the bits,
  //    if any, that were 'unused bits' before enlarging: if value == true,
  //    they must be set.

  if (value && (num_bits > m_num_bits)) {

    const size_type extra_bits = count_extra_bits(); // gps
    if (extra_bits) {
        assert(old_num_blocks >= 1 && old_num_blocks <= m_bits.size());

        // Set them.
        m_bits[old_num_blocks - 1] |= (v << extra_bits); // gps
    }

  }



  m_num_bits = num_bits;
  m_zero_unused_bits();

}

template <typename Block, typename Allocator>
void dynamic_bitset<Block, Allocator>::
clear() // no throw
{
  m_bits.clear();
  m_num_bits = 0;
}


template <typename Block, typename Allocator>
void dynamic_bitset<Block, Allocator>::
push_back(bool bit)
{
  resize(size() + 1);
  set(size() - 1, bit);
}

template <typename Block, typename Allocator>
void dynamic_bitset<Block, Allocator>::
append(Block value) // strong guarantee
{
    // G.P.S. to be reviewed...

    const block_width_type r = count_extra_bits();

    if (r == 0) {
        // the buffer is empty, or all blocks are filled
        m_bits.push_back(value);
    }
    else {
        m_bits.push_back(value >> (bits_per_block - r));
        m_bits[m_bits.size() - 2] |= (value << r); // m_bits.size() >= 2
    }

    m_num_bits += bits_per_block;
    assert(m_check_invariants());

}


//-----------------------------------------------------------------------------
// bitset operations
template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::operator&=(const dynamic_bitset& rhs)
{
    assert(size() == rhs.size());
    for (size_type i = 0; i < num_blocks(); ++i)
        m_bits[i] &= rhs.m_bits[i];
    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::operator|=(const dynamic_bitset& rhs)
{
    assert(size() == rhs.size());
    for (size_type i = 0; i < num_blocks(); ++i)
        m_bits[i] |= rhs.m_bits[i];
    //m_zero_unused_bits();
    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::operator^=(const dynamic_bitset& rhs)
{
    assert(size() == rhs.size());
    for (size_type i = 0; i < this->num_blocks(); ++i)
        m_bits[i] ^= rhs.m_bits[i];
    //m_zero_unused_bits();
    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::operator-=(const dynamic_bitset& rhs)
{
    assert(size() == rhs.size());
    for (size_type i = 0; i < num_blocks(); ++i)
        m_bits[i] &= ~rhs.m_bits[i];
    //m_zero_unused_bits();
    return *this;
}

//
// NOTE:
//  Note that the 'if (r != 0)' is crucial to avoid undefined
//  behavior when the left hand operand of >> isn't promoted to a
//  wider type (because rs would be too large).
//
template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::operator<<=(size_type n)
{
    if (n >= m_num_bits)
        return reset();
    //else
    if (n > 0) {

        size_type    const last = num_blocks() - 1;  // num_blocks() is >= 1
        size_type    const div  = n / bits_per_block; // div is <= last
        block_width_type const r = bit_index(n);
        block_type * const b    = &m_bits[0];

        if (r != 0) {

            block_width_type const rs = bits_per_block - r;

            for (size_type i = last-div; i>0; --i) {
                b[i+div] = (b[i] << r) | (b[i-1] >> rs);
            }
            b[div] = b[0] << r;

        }
        else {
            for (size_type i = last-div; i>0; --i) {
                b[i+div] = b[i];
            }
            b[div] = b[0];
        }


        // zero out div blocks at the less significant end
        std::fill_n(b, div, static_cast<block_type>(0));

        // zero out any 1 bit that flowed into the unused part
        m_zero_unused_bits(); // thanks to Lester Gong


    }

    return *this;


}


//
// NOTE:
//  see the comments to operator <<=
//
template <typename B, typename A>
dynamic_bitset<B, A> & dynamic_bitset<B, A>::operator>>=(size_type n) {
    if (n >= m_num_bits) {
        return reset();
    }
    //else
    if (n>0) {

        size_type  const last  = num_blocks() - 1; // num_blocks() is >= 1
        size_type  const div   = n / bits_per_block;   // div is <= last
        block_width_type const r     = bit_index(n);
        block_type * const b   = &m_bits[0];


        if (r != 0) {

            block_width_type const ls = bits_per_block - r;

            for (size_type i = div; i < last; ++i) {
                b[i-div] = (b[i] >> r) | (b[i+1]  << ls);
            }
            // r bits go to zero
            b[last-div] = b[last] >> r;
        }

        else {
            for (size_type i = div; i <= last; ++i) {
                b[i-div] = b[i];
            }
            // note the '<=': the last iteration 'absorbs'
            // b[last-div] = b[last] >> 0;
        }



        // div blocks are zero filled at the most significant end
        std::fill_n(b + (num_blocks()-div), div, static_cast<block_type>(0));
    }

    return *this;
}


template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
dynamic_bitset<Block, Allocator>::operator<<(size_type n) const
{
    dynamic_bitset r(*this);
    return r <<= n;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
dynamic_bitset<Block, Allocator>::operator>>(size_type n) const
{
    dynamic_bitset r(*this);
    return r >>= n;
}


//-----------------------------------------------------------------------------
// basic bit operations

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::set(size_type pos, bool val)
{
    // [gps]
    //
    // Below we have no set(size_type) function to call when
    // value == true; instead of using a helper, I think
    // overloading set (rather than giving it a default bool
    // argument) would be more elegant.

    assert(pos < m_num_bits);

    if (val)
        m_bits[block_index(pos)] |= bit_mask(pos);
    else
        reset(pos);

    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::set()
{
  std::fill(m_bits.begin(), m_bits.end(), ~Block(0));
  m_zero_unused_bits();
  return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::reset(size_type pos)
{
    assert(pos < m_num_bits);
    #if BOOST_WORKAROUND(__MWERKS__, <= 0x3003) // 8.x
    // CodeWarrior 8 generates incorrect code when the &=~ is compiled,
    // use the |^ variation instead.. <grafik>
    m_bits[block_index(pos)] |= bit_mask(pos);
    m_bits[block_index(pos)] ^= bit_mask(pos);
    #else
    m_bits[block_index(pos)] &= ~bit_mask(pos);
    #endif
    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::reset()
{
  std::fill(m_bits.begin(), m_bits.end(), Block(0));
  return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::flip(size_type pos)
{
    assert(pos < m_num_bits);
    m_bits[block_index(pos)] ^= bit_mask(pos);
    return *this;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>&
dynamic_bitset<Block, Allocator>::flip()
{
    for (size_type i = 0; i < num_blocks(); ++i)
        m_bits[i] = ~m_bits[i];
    m_zero_unused_bits();
    return *this;
}

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::m_unchecked_test(size_type pos) const
{
    return (m_bits[block_index(pos)] & bit_mask(pos)) != 0;
}

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::test(size_type pos) const
{
    assert(pos < m_num_bits);
    return m_unchecked_test(pos);
}

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::any() const
{
    for (size_type i = 0; i < num_blocks(); ++i)
        if (m_bits[i])
            return true;
    return false;
}

template <typename Block, typename Allocator>
inline bool dynamic_bitset<Block, Allocator>::none() const
{
    return !any();
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
dynamic_bitset<Block, Allocator>::operator~() const
{
    dynamic_bitset b(*this);
    b.flip();
    return b;
}


/*

The following is the straightforward implementation of count(), which
we leave here in a comment for documentation purposes.

template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::count() const
{
    size_type sum = 0;
    for (size_type i = 0; i != this->m_num_bits; ++i)
        if (test(i))
            ++sum;
    return sum;
}

The actual algorithm uses a lookup table.


  The basic idea of the method is to pick up X bits at a time
  from the internal array of blocks and consider those bits as
  the binary representation of a number N. Then, to use a table
  of 1<<X elements where table[N] is the number of '1' digits
  in the binary representation of N (i.e. in our X bits).


  In this implementation X is 8 (but can be easily changed: you
  just have to modify the definition of table_width and shrink/enlarge
  the table accordingly - it could be useful, for instance, to expand
  the table to 512 elements on an implementation with 9-bit bytes) and
  the internal array of blocks is seen, if possible, as an array of bytes.
  In practice the "reinterpretation" as array of bytes is possible if and
  only if X >= CHAR_BIT and Block has no padding bits (that would be counted
  together with the "real ones" if we saw the array as array of bytes).
  Otherwise we simply 'extract' X bits at a time from each Block.

*/


template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::count() const
{
    using namespace detail::dynamic_bitset_count_impl;

    const bool no_padding = bits_per_block == CHAR_BIT * sizeof(Block);
    const bool enough_table_width = table_width >= CHAR_BIT;

    typedef mode_to_type< (no_padding && enough_table_width ?
                          access_by_bytes : access_by_blocks) > m;

    return do_count(m_bits.begin(), num_blocks(), Block(0), static_cast<m*>(0));

}


//-----------------------------------------------------------------------------
// conversions


template <typename B, typename A, typename stringT>
void to_string_helper(const dynamic_bitset<B, A> & b, stringT & s,
                      bool dump_all)
{
    typedef typename stringT::traits_type Tr;
    typedef typename stringT::value_type  Ch;

    BOOST_DYNAMIC_BITSET_CTYPE_FACET(Ch, fac, std::locale());
    const Ch zero = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '0');
    const Ch one  = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '1');

    // Note that this function may access (when
    // dump_all == true) bits beyond position size() - 1

    typedef typename dynamic_bitset<B, A>::size_type size_type;

    const size_type len = dump_all?
         dynamic_bitset<B, A>::bits_per_block * b.num_blocks():
         b.size();
    s.assign (len, zero);

    for (size_type i = 0; i < len; ++i) {
        if (b.m_unchecked_test(i))
            Tr::assign(s[len - 1 - i], one);

    }

}


// A comment similar to the one about the constructor from
// basic_string can be done here. Thanks to James Kanze for
// making me (Gennaro) realize this important separation of
// concerns issue, as well as many things about i18n.
//
template <typename Block, typename Allocator, typename stringT> // G.P.S.
inline void
to_string(const dynamic_bitset<Block, Allocator>& b, stringT& s)
{
    to_string_helper(b, s, false);
}


// Differently from to_string this function dumps out
// every bit of the internal representation (may be
// useful for debugging purposes)
//
template <typename B, typename A, typename stringT>
inline void
dump_to_string(const dynamic_bitset<B, A>& b, stringT& s) // G.P.S.
{
    to_string_helper(b, s, true /* =dump_all*/);
}

template <typename Block, typename Allocator, typename BlockOutputIterator>
inline void
to_block_range(const dynamic_bitset<Block, Allocator>& b,
               BlockOutputIterator result)
{
    // note how this copies *all* bits, including the
    // unused ones in the last block (which are zero)
    std::copy(b.m_bits.begin(), b.m_bits.end(), result); // [gps]
}

template <typename Block, typename Allocator>
unsigned long dynamic_bitset<Block, Allocator>::
to_ulong() const
{

  if (m_num_bits == 0)
      return 0; // convention

  // Check for overflows. This may be a performance burden on very
  // large bitsets but is required by the specification, sorry
  if (find_next(ulong_width - 1) != npos)
    throw std::overflow_error("boost::dynamic_bitset::to_ulong overflow");


  // Ok, from now on we can be sure there's no "on" bit beyond
  // the allowed positions

  if (bits_per_block >= ulong_width)
      return m_bits[0];


  size_type last_block = block_index((std::min)(m_num_bits-1, // gps
                                    (size_type)(ulong_width-1)));
  unsigned long result = 0;
  for (size_type i = 0; i <= last_block; ++i) {

    assert((size_type)bits_per_block * i < (size_type)ulong_width); // gps

    unsigned long piece = m_bits[i];
    result |= (piece << (bits_per_block * i));
  }

  return result;

}


template <typename Block, typename Allocator>
inline typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::size() const
{
    return m_num_bits;
}

template <typename Block, typename Allocator>
inline typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::num_blocks() const
{
    return m_bits.size();
}

template <typename Block, typename Allocator>
inline typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::max_size() const
{
    // Semantics of vector<>::max_size() aren't very clear
    // (see lib issue 197) and many library implementations
    // simply return dummy values, _unrelated_ to the underlying
    // allocator.
    //
    // Given these problems, I was tempted to not provide this
    // function at all but the user could need it if he provides
    // his own allocator.
    //

    const size_type m = detail::vector_max_size_workaround(m_bits);

    return m <= (size_type(-1)/bits_per_block) ?
        m * bits_per_block :
        size_type(-1);
}

template <typename Block, typename Allocator>
inline bool dynamic_bitset<Block, Allocator>::empty() const
{
  return size() == 0;
}

#if 0 // gps
template <typename Block, typename Allocator>
inline void dynamic_bitset<Block, Allocator>::reserve(size_type n)
{
    assert(n <= max_size()); // PRE - G.P.S.
    m_bits.reserve(calc_num_blocks(n));
}

template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::capacity() const
{
    // capacity is m_bits.capacity() * bits_per_block
    // unless that one overflows
    const size_type m = static_cast<size_type>(-1);
    const size_type q = m / bits_per_block;

    const size_type c = m_bits.capacity();

    return c <= q ?
        c * bits_per_block :
        m;
}
#endif

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::
is_subset_of(const dynamic_bitset<Block, Allocator>& a) const
{
    assert(size() == a.size());
    for (size_type i = 0; i < num_blocks(); ++i)
        if (m_bits[i] & ~a.m_bits[i])
            return false;
    return true;
}

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::
is_proper_subset_of(const dynamic_bitset<Block, Allocator>& a) const
{
    assert(size() == a.size());
    bool proper = false;
    for (size_type i = 0; i < num_blocks(); ++i) {
        Block bt = m_bits[i], ba = a.m_bits[i];
        if (ba & ~bt)
            proper = true;
        if (bt & ~ba)
            return false;
    }
    return proper;
}

template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::intersects(const dynamic_bitset & b) const
{
    size_type common_blocks = num_blocks() < b.num_blocks()
                              ? num_blocks() : b.num_blocks();

    for(size_type i = 0; i < common_blocks; ++i) {
        if(m_bits[i] & b.m_bits[i])
            return true;
    }
    return false;
}

// --------------------------------
// lookup


// look for the first bit "on", starting
// from the block with index first_block
//
template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::m_do_find_from(size_type first_block) const
{
    size_type i = first_block;

    // skip null blocks
    while (i < num_blocks() && m_bits[i] == 0)
        ++i;

    if (i >= num_blocks())
        return npos; // not found

    return i * bits_per_block + boost::lowest_bit(m_bits[i]);

}


template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::find_first() const
{
    return m_do_find_from(0);
}


template <typename Block, typename Allocator>
typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::find_next(size_type pos) const
{

    const size_type sz = size();
    if (pos >= (sz-1) || sz == 0)
        return npos;

    ++pos;

    const size_type blk = block_index(pos);
    const block_width_type ind = bit_index(pos);

    // mask out bits before pos
    const Block fore = m_bits[blk] & ( ~Block(0) << ind );

    return fore?
        blk * bits_per_block + lowest_bit(fore)
        :
        m_do_find_from(blk + 1);

}



//-----------------------------------------------------------------------------
// comparison

template <typename Block, typename Allocator>
bool operator==(const dynamic_bitset<Block, Allocator>& a,
                const dynamic_bitset<Block, Allocator>& b)
{
    return (a.m_num_bits == b.m_num_bits)
           && (a.m_bits == b.m_bits); // [gps]
}

template <typename Block, typename Allocator>
inline bool operator!=(const dynamic_bitset<Block, Allocator>& a,
                       const dynamic_bitset<Block, Allocator>& b)
{
    return !(a == b);
}

template <typename Block, typename Allocator>
bool operator<(const dynamic_bitset<Block, Allocator>& a,
               const dynamic_bitset<Block, Allocator>& b)
{
    assert(a.size() == b.size());
    typedef typename dynamic_bitset<Block, Allocator>::size_type size_type;

    if (a.size() == 0)
      return false;

    // Since we are storing the most significant bit
    // at pos == size() - 1, we need to do the comparisons in reverse.

    // Compare a block at a time
    for (size_type i = a.num_blocks() - 1; i > 0; --i)
      if (a.m_bits[i] < b.m_bits[i])
        return true;
      else if (a.m_bits[i] > b.m_bits[i])
        return false;

    if (a.m_bits[0] < b.m_bits[0])
      return true;
    else
      return false;
}

template <typename Block, typename Allocator>
inline bool operator<=(const dynamic_bitset<Block, Allocator>& a,
                       const dynamic_bitset<Block, Allocator>& b)
{
    return !(a > b);
}

template <typename Block, typename Allocator>
inline bool operator>(const dynamic_bitset<Block, Allocator>& a,
                      const dynamic_bitset<Block, Allocator>& b)
{
    return b < a;
}

template <typename Block, typename Allocator>
inline bool operator>=(const dynamic_bitset<Block, Allocator>& a,
                       const dynamic_bitset<Block, Allocator>& b)
{
    return !(a < b);
}

//-----------------------------------------------------------------------------
// stream operations

#ifdef BOOST_OLD_IOSTREAMS
template < typename Block, typename Alloc>
std::ostream&
operator<<(std::ostream& os, const dynamic_bitset<Block, Alloc>& b)
{
    // NOTE: since this is aimed at "classic" iostreams, exception
    // masks on the stream are not supported. The library that
    // ships with gcc 2.95 has an exceptions() member function but
    // nothing is actually implemented; not even the class ios::failure.

    using namespace std;

    const ios::iostate ok = ios::goodbit;
    ios::iostate err = ok;

    if (os.opfx()) { // gps

        //try
        typedef typename dynamic_bitset<Block, Alloc>::size_type bitsetsize_type;

        const bitsetsize_type sz = b.size();
        std::streambuf * buf = os.rdbuf();
        size_t npad = os.width() <= 0  // careful: os.width() is signed (and can be < 0)
            || (bitsetsize_type) os.width() <= sz? 0 : os.width() - sz; //- gps

        const char fill_char = os.fill();
        const ios::fmtflags adjustfield = os.flags() & ios::adjustfield;

        // if needed fill at left; pad is decresed along the way
        if (adjustfield != ios::left) {
            for (; 0 < npad; --npad)
                if (fill_char != buf->sputc(fill_char)) {
                    err |= ios::failbit;   // gps
                    break;
                }
        }

        if (err == ok) {
            // output the bitset
            for (bitsetsize_type i = b.size(); 0 < i; --i) {// G.P.S.
                const char dig = b.test(i-1)? '1' : '0';
                if (EOF == buf->sputc(dig)) { // ok?? gps
                    err |= ios::failbit;
                    break;
                }
            }
        }

        if (err == ok) {
            // if needed fill at right
            for (; 0 < npad; --npad) {
                if (fill_char != buf->sputc(fill_char)) {
                    err |= ios::failbit;
                    break;
                }
            }
        }

        os.osfx();
        os.width(0);

    } // if opfx

    if(err != ok)
        os.setstate(err); // assume this does NOT throw - gps
    return os;

}
#else

template <typename Ch, typename Tr, typename Block, typename Alloc>
std::basic_ostream<Ch, Tr>&
operator<<(std::basic_ostream<Ch, Tr>& os,
           const dynamic_bitset<Block, Alloc>& b)
{

    using namespace std;

    const ios_base::iostate ok = ios_base::goodbit;
    ios_base::iostate err = ok;

    typename basic_ostream<Ch, Tr>::sentry cerberos(os);
    if (cerberos) {

        BOOST_DYNAMIC_BITSET_CTYPE_FACET(Ch, fac, os.getloc());
        const Ch zero = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '0');
        const Ch one  = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '1');

        try {

            typedef typename dynamic_bitset<Block, Alloc>::size_type bitsetsize_type;
            typedef basic_streambuf<Ch, Tr> buffer_type; // G.P.S.

            buffer_type * buf = os.rdbuf();
            size_t npad = os.width() <= 0  // careful: os.width() is signed (and can be < 0)
                || (bitsetsize_type) os.width() <= b.size()? 0 : os.width() - b.size(); //- G.P.S.

            const Ch fill_char = os.fill();
            const ios_base::fmtflags adjustfield = os.flags() & ios_base::adjustfield;

            // if needed fill at left; pad is decresed along the way
            if (adjustfield != ios_base::left) {
                for (; 0 < npad; --npad)
                    if (Tr::eq_int_type(Tr::eof(), buf->sputc(fill_char))) {
                          err |= ios_base::failbit;   // G.P.S.
                          break;
                    }
            }

            if (err == ok) {
                // output the bitset
                for (bitsetsize_type i = b.size(); 0 < i; --i) {// G.P.S.
                    typename buffer_type::int_type
                        ret = buf->sputc(b.test(i-1)? one : zero);
                    if (Tr::eq_int_type(Tr::eof(), ret)) {
                        err |= ios_base::failbit;
                        break;
                    }
                }
            }

            if (err == ok) {
                // if needed fill at right
                for (; 0 < npad; --npad) {
                    if (Tr::eq_int_type(Tr::eof(), buf->sputc(fill_char))) {
                        err |= ios_base::failbit;
                        break;
                    }
                }
            }


            os.width(0);

        } catch (...) { // see std 27.6.1.1/4
            bool rethrow = false;
            try { os.setstate(ios_base::failbit); } catch (...) { rethrow = true; }

            if (rethrow)
                throw;
        }
    }

    if(err != ok)
        os.setstate(err); // may throw exception
    return os;

}
#endif


#ifdef BOOST_OLD_IOSTREAMS

    // gps - A sentry-like class that calls isfx in its
    // destructor. Necessary because bit_appender::do_append may throw.
    class pseudo_sentry {
        std::istream & m_r;
        const bool m_ok;
    public:
        explicit pseudo_sentry(std::istream & r) : m_r(r), m_ok(r.ipfx(0)) { }
        ~pseudo_sentry() { m_r.isfx(); }
        operator bool() const { return m_ok; }
    };

template <typename Block, typename Alloc>
std::istream&
operator>>(std::istream& is, dynamic_bitset<Block, Alloc>& b)
{

// Extractor for classic IO streams (libstdc++ < 3.0)
// ----------------------------------------------------//
//  It's assumed that the stream buffer functions, and
//  the stream's setstate() _cannot_ throw.


    typedef dynamic_bitset<Block, Alloc> bitset_type;
    typedef typename bitset_type::size_type size_type;

    std::ios::iostate err = std::ios::goodbit; // gps
    pseudo_sentry cerberos(is); // skips whitespaces
    if(cerberos) {

        b.clear();

        const std::streamsize w = is.width();
        const size_type limit = w > 0 && static_cast<size_type>(w) < b.max_size()// gps
                                                         ? w : b.max_size();
        typename bitset_type::bit_appender appender(b);
        std::streambuf * buf = is.rdbuf();
        for(int c = buf->sgetc(); appender.get_count() < limit; c = buf->snextc() ) {

            if (c == EOF) {
                err |= std::ios::eofbit; // G.P.S.
                break;
            }
            else if (char(c) != '0' && char(c) != '1')
                break; // non digit character

            else {
                try {
                    //throw std::bad_alloc(); // gps
                    appender.do_append(char(c) == '1');
                }
                catch(...) {
                    is.setstate(std::ios::failbit); // assume this can't throw
                    throw;
                }
            }

        } // for
    }

    is.width(0); // gps
    if (b.size() == 0)
        err |= std::ios::failbit;
    if (err != std::ios::goodbit) // gps
        is.setstate (err); // may throw

    return is;
}

#else // BOOST_OLD_IOSTREAMS

template <typename Ch, typename Tr, typename Block, typename Alloc>
std::basic_istream<Ch, Tr>&
operator>>(std::basic_istream<Ch, Tr>& is, dynamic_bitset<Block, Alloc>& b)
{

    using namespace std;

    typedef dynamic_bitset<Block, Alloc> bitset_type;
    typedef typename bitset_type::size_type size_type;

    const streamsize w = is.width();
    const size_type limit = 0 < w && static_cast<size_type>(w) < b.max_size()? // gps
                                         w : b.max_size();

    ios_base::iostate err = ios_base::goodbit; // gps
    typename basic_istream<Ch, Tr>::sentry cerberos(is); // skips whitespaces
    if(cerberos) {

        // in accordance with prop. resol. of lib DR 303 [last checked 4 Feb 2004]
        BOOST_DYNAMIC_BITSET_CTYPE_FACET(Ch, fac, is.getloc());
        const Ch zero = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '0');
        const Ch one  = BOOST_DYNAMIC_BITSET_WIDEN_CHAR(fac, '1');

        b.clear();
        try {
            typename bitset_type::bit_appender appender(b);
            basic_streambuf <Ch, Tr> * buf = is.rdbuf();
            typename Tr::int_type c = buf->sgetc(); // G.P.S.
            for( ; appender.get_count() < limit; c = buf->snextc() ) {

                if (Tr::eq_int_type(Tr::eof(), c)) {
                    err |= ios_base::eofbit; // G.P.S.
                    break;
                }
                else {
                    const Ch to_c = Tr::to_char_type(c);
                    const bool is_one = Tr::eq(to_c, one);

                    if (!is_one && !Tr::eq(to_c, zero))
                        break; // non digit character

                    appender.do_append(is_one);

                }

            } // for
        }
        catch (...) {
            // catches from stream buf, or from vector:
            //
            // bits_stored bits have been extracted and stored, and
            // either no further character is extractable or we can't
            // append to the underlying vector (out of memory) gps

            bool rethrow = false;   // see std 27.6.1.1/4
            try { is.setstate(ios_base::badbit); }
            catch(...) { rethrow = true; }

            if (rethrow)
                throw;

        }
    }

    is.width(0); // gps
    if (b.size() == 0 /*|| !cerberos*/)
        err |= ios_base::failbit;
    if (err != ios_base::goodbit) // gps
        is.setstate (err); // may throw

    return is;

}


#endif


//-----------------------------------------------------------------------------
// bitset operations

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator&(const dynamic_bitset<Block, Allocator>& x,
          const dynamic_bitset<Block, Allocator>& y)
{
    dynamic_bitset<Block, Allocator> b(x);
    return b &= y;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator|(const dynamic_bitset<Block, Allocator>& x,
          const dynamic_bitset<Block, Allocator>& y)
{
    dynamic_bitset<Block, Allocator> b(x);
    return b |= y;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator^(const dynamic_bitset<Block, Allocator>& x,
          const dynamic_bitset<Block, Allocator>& y)
{
    dynamic_bitset<Block, Allocator> b(x);
    return b ^= y;
}

template <typename Block, typename Allocator>
dynamic_bitset<Block, Allocator>
operator-(const dynamic_bitset<Block, Allocator>& x,
          const dynamic_bitset<Block, Allocator>& y)
{
    dynamic_bitset<Block, Allocator> b(x);
    return b -= y;
}

//-----------------------------------------------------------------------------
// namespace scope swap

template<typename Block, typename Allocator>
inline void
swap(dynamic_bitset<Block, Allocator>& left,
     dynamic_bitset<Block, Allocator>& right) // no throw
{
    left.swap(right); // gps
}


//-----------------------------------------------------------------------------
// private (on conforming compilers) member functions


template <typename Block, typename Allocator>
inline typename dynamic_bitset<Block, Allocator>::size_type
dynamic_bitset<Block, Allocator>::calc_num_blocks(size_type num_bits)
{
    return num_bits / bits_per_block
           + static_cast<int>( num_bits % bits_per_block != 0 );
}

// gives a reference to the highest block
//
template <typename Block, typename Allocator>
inline Block& dynamic_bitset<Block, Allocator>::m_highest_block()
{
    return const_cast<Block &>
           (static_cast<const dynamic_bitset *>(this)->m_highest_block());
}

// gives a const-reference to the highest block
//
template <typename Block, typename Allocator>
inline const Block& dynamic_bitset<Block, Allocator>::m_highest_block() const
{
    assert(size() > 0 && num_blocks() > 0);
    return m_bits.back();
}


// If size() is not a multiple of bits_per_block
// then not all the bits in the last block are used.
// This function resets the unused bits (convenient
// for the implementation of many member functions)
//
template <typename Block, typename Allocator>
inline void dynamic_bitset<Block, Allocator>::m_zero_unused_bits()
{
    assert (num_blocks() == calc_num_blocks(m_num_bits));

    // if != 0 this is the number of bits used in the last block
    const block_width_type extra_bits = count_extra_bits();

    if (extra_bits != 0)
        m_highest_block() &= ~(~static_cast<Block>(0) << extra_bits);

}

// check class invariants
template <typename Block, typename Allocator>
bool dynamic_bitset<Block, Allocator>::m_check_invariants() const
{
    const block_width_type extra_bits = count_extra_bits();
    if (extra_bits > 0) {
        block_type const mask = (~static_cast<Block>(0) << extra_bits);
        if ((m_highest_block() & mask) != 0)
            return false;
    }
    if (m_bits.size() > m_bits.capacity() || num_blocks() != calc_num_blocks(size()))
        return false;

    return true;

}


} // namespace boost


#undef BOOST_BITSET_CHAR

#endif // include guard

