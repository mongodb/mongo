///////////////////////////////////////////////////////////////////////////////
/// \file sub_match.hpp
/// Contains the definition of the class template sub_match\<\>
/// and associated helper functions
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_SUB_MATCH_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_SUB_MATCH_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <iosfwd>
#include <string>
#include <utility>
#include <iterator>
#include <algorithm>
#include <boost/iterator/iterator_traits.hpp>

//{{AFX_DOC_COMMENT
///////////////////////////////////////////////////////////////////////////////
// This is a hack to get Doxygen to show the inheritance relation between
// sub_match<T> and std::pair<T,T>.
#ifdef BOOST_XPRESSIVE_DOXYGEN_INVOKED
/// INTERNAL ONLY
namespace std
{
    /// INTERNAL ONLY
    template<typename, typename> struct pair {};
}
#endif
//}}AFX_DOC_COMMENT

namespace boost { namespace xpressive
{

///////////////////////////////////////////////////////////////////////////////
// sub_match
//
/// \brief Class template sub_match denotes the sequence of characters matched by a particular marked sub-expression.
///
/// When the marked sub-expression denoted by an object of type sub_match\<\> participated in a
/// regular expression match then member matched evaluates to true, and members first and second
/// denote the range of characters [first,second) which formed that match. Otherwise matched is false,
/// and members first and second contained undefined values.
///
/// If an object of type sub_match\<\> represents sub-expression 0 - that is to say the whole match -
/// then member matched is always true, unless a partial match was obtained as a result of the flag
/// match_partial being passed to a regular expression algorithm, in which case member matched is
/// false, and members first and second represent the character range that formed the partial match.
template<typename BidiIter>
struct sub_match
  : std::pair<BidiIter, BidiIter>
{
private:
    struct dummy { int i_; };
    typedef int dummy::*bool_type;

public:
    typedef typename iterator_value<BidiIter>::type value_type;
    typedef typename iterator_difference<BidiIter>::type difference_type;
    typedef std::basic_string<value_type> string_type;
    typedef BidiIter iterator;

    explicit sub_match(BidiIter first = BidiIter(), BidiIter second = BidiIter(), bool matched_ = false)
      : std::pair<BidiIter, BidiIter>(first, second)
      , matched(matched_)
    {
    }

    string_type str() const
    {
        return this->matched ? string_type(this->first, this->second) : string_type();
    }

    operator string_type() const
    {
        return this->matched ? string_type(this->first, this->second) : string_type();
    }

    difference_type length() const
    {
        return this->matched ? std::distance(this->first, this->second) : 0;
    }

    operator bool_type() const
    {
        return this->matched ? &dummy::i_ : 0;
    }

    bool operator !() const
    {
        return !this->matched;
    }

    /// \brief Performs a lexicographic string comparison
    /// \param str the string against which to compare
    /// \return the results of (*this).str().compare(str)
    int compare(string_type const &str) const
    {
        return this->str().compare(str);
    }

    /// \overload
    int compare(sub_match const &sub) const
    {
        return this->str().compare(sub.str());
    }

    /// \overload
    int compare(value_type const *ptr) const
    {
        return this->str().compare(ptr);
    }

    /// \brief true if this sub-match participated in the full match.
    bool matched;
};

///////////////////////////////////////////////////////////////////////////////
/// \brief insertion operator for sending sub-matches to ostreams
/// \param sout output stream.
/// \param sub sub_match object to be written to the stream.
/// \return sout \<\< sub.str()
template<typename BidiIter, typename Char, typename Traits>
inline std::basic_ostream<Char, Traits> &operator <<
(
    std::basic_ostream<Char, Traits> &sout
  , sub_match<BidiIter> const &sub
)
{
    typedef typename iterator_value<BidiIter>::type char_type;
    if(sub.matched)
    {
        std::ostream_iterator<char_type, Char, Traits> iout(sout);
        std::copy(sub.first, sub.second, iout);
    }
    return sout;
}


// BUGBUG make these more efficient

template<typename BidiIter>
bool operator == (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs) == 0;
}

template<typename BidiIter>
bool operator != (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs) != 0;
}

template<typename BidiIter>
bool operator < (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs) < 0;
}

template<typename BidiIter>
bool operator <= (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs) <= 0;
}

template<typename BidiIter>
bool operator >= (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs)>= 0;
}

template<typename BidiIter>
bool operator> (sub_match<BidiIter> const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs.compare(rhs)> 0;
}

template<typename BidiIter>
bool operator == (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs == rhs.str();
}

template<typename BidiIter>
bool operator != (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs != rhs.str();
}

template<typename BidiIter>
bool operator < (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs < rhs.str();
}

template<typename BidiIter>
bool operator> (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs> rhs.str();
}

template<typename BidiIter>
bool operator >= (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs >= rhs.str();
}

template<typename BidiIter>
bool operator <= (typename iterator_value<BidiIter>::type const *lhs, sub_match<BidiIter> const &rhs)
{
    return lhs <= rhs.str();
}

template<typename BidiIter>
bool operator == (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str() == rhs;
}

template<typename BidiIter>
bool operator != (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str() != rhs;
}

template<typename BidiIter>
bool operator < (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str() < rhs;
}

template<typename BidiIter>
bool operator> (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str()> rhs;
}

template<typename BidiIter>
bool operator >= (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str()>= rhs;
}

template<typename BidiIter>
bool operator <= (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const *rhs)
{
    return lhs.str() <= rhs;
}

template<typename BidiIter>
bool operator == (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs == rhs.str();
}

template<typename BidiIter>
bool operator != (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs != rhs.str();
}

template<typename BidiIter>
bool operator < (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs < rhs.str();
}

template<typename BidiIter>
bool operator> (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs> rhs.str();
}

template<typename BidiIter>
bool operator >= (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs >= rhs.str();
}

template<typename BidiIter>
bool operator <= (typename iterator_value<BidiIter>::type const &lhs, sub_match<BidiIter> const &rhs)
{
    return lhs <= rhs.str();
}

template<typename BidiIter>
bool operator == (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str() == rhs;
}

template<typename BidiIter>
bool operator != (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str() != rhs;
}

template<typename BidiIter>
bool operator < (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str() < rhs;
}

template<typename BidiIter>
bool operator> (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str()> rhs;
}

template<typename BidiIter>
bool operator >= (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str()>= rhs;
}

template<typename BidiIter>
bool operator <= (sub_match<BidiIter> const &lhs, typename iterator_value<BidiIter>::type const &rhs)
{
    return lhs.str() <= rhs;
}

}} // namespace boost::xpressive

#endif
