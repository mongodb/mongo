///////////////////////////////////////////////////////////////////////////////
/// \file match_results.hpp
/// Contains the definition of the match_results type and associated helpers.
/// The match_results type holds the results of a regex_match() or
/// regex_search() operation.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_MATCH_RESULTS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_MATCH_RESULTS_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <iterator>
#include <boost/assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/iterator_adaptors.hpp>
#if BOOST_ITERATOR_ADAPTORS_VERSION >= 0x0200
# include <boost/iterator/filter_iterator.hpp>
#endif
#include <boost/xpressive/regex_constants.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/sub_match_vector.hpp>
#include <boost/xpressive/detail/utility/sequence_stack.hpp>
#include <boost/xpressive/detail/core/results_cache.hpp>
#include <boost/xpressive/detail/core/action_state.hpp>
#include <boost/xpressive/detail/utility/literals.hpp>
#include <boost/xpressive/detail/utility/algorithm.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// results_extras
//
template<typename BidiIter>
struct results_extras
{
    sequence_stack<sub_match_impl<BidiIter> > sub_match_stack_;
    results_cache<BidiIter> results_cache_;
};

///////////////////////////////////////////////////////////////////////////////
// results_traits
//
template<typename Char>
struct results_traits
{
    static int value(Char ch, int radix = 10)
    {
        BOOST_ASSERT(10 == radix);
        if(ch >= BOOST_XPR_CHAR_(Char, '0') && ch <= BOOST_XPR_CHAR_(Char, '9'))
        {
            return ch - BOOST_XPR_CHAR_(Char, '0');
        }
        return -1;
    }
};

} // namespace detail

///////////////////////////////////////////////////////////////////////////////
// match_results
/// \brief Class template match_results\<\> holds the results of a regex_match() or a
/// regex_search() as a collection of sub_match objects.
///
/// Class template match_results\<\> denotes a collection of sequences representing the result of
/// a regular expression match. Storage for the collection is allocated and freed as necessary by
/// the member functions of class match_results\<\>.
///
/// The class template match_results\<\> conforms to the requirements of a Sequence, as specified
/// in (lib.sequence.reqmts), except that only operations defined for const-qualified Sequences are
/// supported.
template<typename BidiIter>
struct match_results
{
private:
    struct dummy { int i_; };
    typedef int dummy::*bool_type;

public:
    typedef typename iterator_value<BidiIter>::type char_type;
    typedef std::basic_string<char_type> string_type;
    typedef std::size_t size_type;
    typedef sub_match<BidiIter> value_type;
    typedef typename iterator_difference<BidiIter>::type difference_type;
    typedef value_type const &reference;
    typedef value_type const &const_reference;

    typedef typename detail::sub_match_vector<BidiIter>::iterator iterator;
    typedef typename detail::sub_match_vector<BidiIter>::const_iterator const_iterator;
    typedef typename detail::nested_results<BidiIter> nested_results_type;

    /// \post regex_id() == 0
    /// \post size()     == 0
    /// \post empty()    == true
    match_results()
      : regex_id_(0)
      , sub_matches_()
      , base_()
      , prefix_()
      , suffix_()
      , nested_results_()
      , action_state_()
      , extras_ptr_()
    {
    }

    /// \param that The match_results object to copy
    /// \post regex_id()  == that.regex_id().
    /// \post size()      == that.size().
    /// \post empty()     == that.empty().
    /// \post str(n)      == that.str(n) for all positive integers n \< that.size().
    /// \post prefix()    == that.prefix().
    /// \post suffix()    == that.suffix().
    /// \post (*this)[n]  == that[n] for all positive integers n \< that.size().
    /// \post length(n)   == that.length(n) for all positive integers n \< that.size().
    /// \post position(n) == that.position(n) for all positive integers n \< that.size().
    match_results(match_results<BidiIter> const &that)
      : regex_id_(that.regex_id_)
      , sub_matches_()
      , base_()
      , prefix_()
      , suffix_()
      , nested_results_()
      , action_state_(that.action_state_)
      , extras_ptr_()
    {
        if(that)
        {
            extras_type &extras = this->get_extras_();
            std::size_t size = that.sub_matches_.size();
            detail::sub_match_impl<BidiIter> *sub_matches = extras.sub_match_stack_.push_sequence(size);
            detail::core_access<BidiIter>::init_sub_match_vector(this->sub_matches_, sub_matches, size, that.sub_matches_);

            // BUGBUG this doesn't share the extras::sequence_stack
            this->nested_results_ = that.nested_results_;
            this->prefix_ = that.prefix_;
            this->suffix_ = that.suffix_;
            this->base_ = that.base_;
        }
    }

    ~match_results()
    {
    }

    /// \param that The match_results object to copy.
    /// \post regex_id()  == that.regex_id().
    /// \post size()      == that.size().
    /// \post empty()     == that.empty().
    /// \post str(n)      == that.str(n) for all positive integers n \< that.size().
    /// \post prefix()    == that.prefix().
    /// \post suffix()    == that.suffix().
    /// \post (*this)[n]  == that[n] for all positive integers n \< that.size().
    /// \post length(n)   == that.length(n) for all positive integers n \< that.size().
    /// \post position(n) == that.position(n) for all positive integers n \< that.size().
    match_results<BidiIter> &operator =(match_results<BidiIter> const &that)
    {
        match_results<BidiIter>(that).swap(*this);
        return *this;
    }

    /// Returns the number of sub_match elements stored in *this.
    ///
    size_type size() const
    {
        return this->sub_matches_.size();
    }

    /// Returns size() == 0.
    ///
    bool empty() const
    {
        return 0 == this->size();
    }

    /// Returns (*this)[sub].length().
    ///
    difference_type length(size_type sub = 0) const
    {
        return (*this)[ sub ].length();
    }

    /// If !(*this)[sub].matched then returns -1. Otherwise returns std::distance(base, (*this)[sub].first),
    /// where base is the start iterator of the sequence that was searched. [Note – unless this is part
    /// of a repeated search with a regex_iterator then base is the same as prefix().first – end note]
    difference_type position(size_type sub = 0) const
    {
        return (*this)[ sub ].matched ? std::distance(this->base_, (*this)[ sub ].first) : -1;
    }

    /// Returns string_type((*this)[sub]).
    ///
    string_type str(size_type sub = 0) const
    {
        return (*this)[ sub ].str();
    }

    /// Returns a reference to the sub_match object representing the sequence that
    /// matched marked sub-expression sub. If sub == 0 then returns a reference to a sub_match object
    /// representing the sequence that matched the whole regular expression.
    /// \pre sub \< (*this).size().
    const_reference operator [](size_type sub) const
    {
        return this->sub_matches_[ sub ];
    }

    /// \overload
    ///
    const_reference operator [](detail::mark_tag const &mark) const
    {
        return this->sub_matches_[ detail::get_mark_number(mark) ];
    }

    /// Returns a reference to the sub_match object representing the character sequence from
    /// the start of the string being matched/searched, to the start of the match found.
    ///
    const_reference prefix() const
    {
        return this->prefix_;
    }

    /// Returns a reference to the sub_match object representing the character sequence from
    /// the end of the match found to the end of the string being matched/searched.
    ///
    const_reference suffix() const
    {
        return this->suffix_;
    }

    /// Returns a starting iterator that enumerates over all the marked sub-expression matches
    /// stored in *this.
    ///
    const_iterator begin() const
    {
        return this->sub_matches_.begin();
    }

    /// Returns a terminating iterator that enumerates over all the marked sub-expression
    /// matches stored in *this.
    ///
    const_iterator end() const
    {
        return this->sub_matches_.end();
    }

    /// Returns a true value if(*this)[0].matched, else returns a false value.
    ///
    operator bool_type() const
    {
        return (*this)[ 0 ].matched ? &dummy::i_ : 0;
    }

    /// Returns true if empty() || !(*this)[0].matched, else returns false.
    ///
    bool operator !() const
    {
        return this->empty() || !(*this)[ 0 ].matched;
    }

    /// Returns the id of the basic_regex object most recently used with this match_results object.
    ///
    regex_id_type regex_id() const
    {
        return this->regex_id_;
    }

    /// Returns a Sequence of nested match_results elements.
    ///
    nested_results_type const &nested_results() const
    {
        return this->nested_results_;
    }

    /// Copies the character sequence [fmt.begin(), fmt.end()) to OutputIterator out. For each format
    /// specifier or escape sequence in fmt, replace that sequence with either the character(s) it
    /// represents, or the sequence within *this to which it refers. The bitmasks specified in flags
    /// determines what format specifiers or escape sequences are recognized, by default this is the
    /// format used by ECMA-262, ECMAScript Language Specification, Chapter 15 part 5.4.11 String.prototype.replace.
    template<typename OutputIterator>
    OutputIterator format
    (
        OutputIterator out
      , const string_type &fmt
      , regex_constants::match_flag_type flags = regex_constants::format_default
    ) const
    {
        detail::results_traits<char_type> traits;
        typename string_type::const_iterator cur = fmt.begin(), end = fmt.end();

        if(0 != (regex_constants::format_literal & flags))
        {
            out = std::copy(cur, end, out);
        }
        else while(cur != end)
        {
            if(BOOST_XPR_CHAR_(char_type, '$') != *cur)
            {
                *out++ = *cur++;
            }
            else if(++cur == end)
            {
                *out++ = BOOST_XPR_CHAR_(char_type, '$');
            }
            else if(BOOST_XPR_CHAR_(char_type, '$') == *cur)
            {
                *out++ = *cur++;
            }
            else if(BOOST_XPR_CHAR_(char_type, '&') == *cur) // whole match
            {
                ++cur;
                out = std::copy((*this)[ 0 ].first, (*this)[ 0 ].second, out);
            }
            else if(BOOST_XPR_CHAR_(char_type, '`') == *cur) // prefix
            {
                ++cur;
                out = std::copy(this->prefix().first, this->prefix().second, out);
            }
            else if(BOOST_XPR_CHAR_(char_type, '\'') == *cur) // suffix
            {
                ++cur;
                out = std::copy(this->suffix().first, this->suffix().second, out);
            }
            else if(-1 != traits.value(*cur, 10)) // a sub-match
            {
                int max = static_cast<int>(this->size() - 1);
                int br_nbr = detail::toi(cur, end, traits, 10, max);
                detail::ensure(0 != br_nbr, regex_constants::error_subreg, "invalid back-reference");
                out = std::copy((*this)[ br_nbr ].first, (*this)[ br_nbr ].second, out);
            }
            else
            {
                *out++ = BOOST_XPR_CHAR_(char_type, '$');
                *out++ = *cur++;
            }
        }

        return out;
    }

    /// Returns a copy of the string fmt. For each format specifier or escape sequence in fmt,
    /// replace that sequence with either the character(s) it represents, or the sequence within
    /// *this to which it refers. The bitmasks specified in flags determines what format specifiers
    /// or escape sequences are recognized, by default this is the format used by ECMA-262,
    /// ECMAScript Language Specification, Chapter 15 part 5.4.11 String.prototype.replace.
    string_type format(const string_type &fmt, regex_constants::match_flag_type flags = regex_constants::format_default) const
    {
        string_type result;
        result.reserve(fmt.length() * 2);
        this->format(std::back_inserter(result), fmt, flags);
        return result;
    }

    /// Swaps the contents of two match_results objects. Guaranteed not to throw.
    /// \param that The match_results object to swap with.
    /// \post *this contains the sequence of matched sub-expressions that were in that,
    /// that contains the sequence of matched sub-expressions that were in *this.
    /// \throw nothrow
    void swap(match_results<BidiIter> &that) // throw()
    {
        std::swap(this->regex_id_, that.regex_id_);
        this->sub_matches_.swap(that.sub_matches_);
        std::swap(this->base_, that.base_);
        std::swap(this->prefix_, that.prefix_);
        std::swap(this->suffix_, that.suffix_);
        this->nested_results_.swap(that.nested_results_);
        std::swap(this->action_state_, that.action_state_);
        this->extras_ptr_.swap(that.extras_ptr_);
    }

    /// INTERNAL ONLY
    match_results<BidiIter> const &operator ()(regex_id_type regex_id, size_type index = 0) const
    {
        // BUGBUG this is linear, make it O(1)
        static match_results<BidiIter> const s_null;

        regex_id_filter_predicate<BidiIter> pred(regex_id);
        typename nested_results_type::const_iterator
            begin = this->nested_results_.begin()
          , end = this->nested_results_.end()
          , cur = detail::find_nth_if(begin, end, index, pred);

        return (cur == end) ? s_null : *cur;
    }

    /// INTERNAL ONLY
    match_results<BidiIter> const &operator ()(basic_regex<BidiIter> const &rex, std::size_t index = 0) const
    {
        return (*this)(rex.regex_id(), index);
    }

    // state:
    /// INTERNAL ONLY
    template<typename State>
    void set_action_state(State &state)
    {
        this->action_state_.set(state);
    }

    /// INTERNAL ONLY
    template<typename State>
    State &get_action_state() const
    {
        return this->action_state_.BOOST_NESTED_TEMPLATE get<State>();
    }

private:

    friend struct detail::core_access<BidiIter>;
    typedef detail::results_extras<BidiIter> extras_type;

    /// INTERNAL ONLY
    void init_
    (
        regex_id_type regex_id
      , detail::sub_match_impl<BidiIter> *sub_matches
      , size_type size
    )
    {
        this->regex_id_ = regex_id;
        detail::core_access<BidiIter>::init_sub_match_vector(this->sub_matches_, sub_matches, size);
    }

    /// INTERNAL ONLY
    extras_type &get_extras_()
    {
        if(!this->extras_ptr_)
        {
            this->extras_ptr_.reset(new extras_type);
        }

        return *this->extras_ptr_;
    }

    /// INTERNAL ONLY
    void set_prefix_suffix_(BidiIter begin, BidiIter end)
    {
        this->base_ = begin;

        this->prefix_.first = begin;
        this->prefix_.second = (*this)[ 0 ].first;
        this->prefix_.matched = this->prefix_.first != this->prefix_.second;

        this->suffix_.first = (*this)[ 0 ].second;
        this->suffix_.second = end;
        this->suffix_.matched = this->suffix_.first != this->suffix_.second;

        typename nested_results_type::iterator ibegin = this->nested_results_.begin();
        typename nested_results_type::iterator iend = this->nested_results_.end();
        for( ; ibegin != iend; ++ibegin )
        {
            ibegin->set_prefix_suffix_(begin, end);
        }
    }

    /// INTERNAL ONLY
    void reset_()
    {
        detail::core_access<BidiIter>::init_sub_match_vector(this->sub_matches_, 0, 0);
    }

    /// INTERNAL ONLY
    void set_base_(BidiIter base)
    {
        this->base_ = base;

        typename nested_results_type::iterator ibegin = this->nested_results_.begin();
        typename nested_results_type::iterator iend = this->nested_results_.end();
        for( ; ibegin != iend; ++ibegin )
        {
            ibegin->set_base_(base);
        }
    }

    regex_id_type regex_id_;
    detail::sub_match_vector<BidiIter> sub_matches_;
    BidiIter base_;
    sub_match<BidiIter> prefix_;
    sub_match<BidiIter> suffix_;
    nested_results_type nested_results_;
    detail::action_state action_state_;
    shared_ptr<extras_type> extras_ptr_;
};

///////////////////////////////////////////////////////////////////////////////
// action_state_cast
/// INTERNAL ONLY
template<typename State, typename BidiIter>
inline State &action_state_cast(match_results<BidiIter> const &what)
{
    return what.BOOST_NESTED_TEMPLATE get_action_state<State>();
}

///////////////////////////////////////////////////////////////////////////////
// regex_id_filter_predicate
//
template<typename BidiIter>
struct regex_id_filter_predicate
  : std::unary_function<match_results<BidiIter>, bool>
{
    regex_id_filter_predicate(regex_id_type regex_id)
      : regex_id_(regex_id)
    {
    }

    bool operator ()(match_results<BidiIter> const &res) const
    {
        return this->regex_id_ == res.regex_id();
    }

private:

    regex_id_type regex_id_;
};

}} // namespace boost::xpressive

#endif
