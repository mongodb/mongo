///////////////////////////////////////////////////////////////////////////////
/// \file basic_regex.hpp
/// Contains the definition of the basic_regex\<\> class template and its
/// associated helper functions.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_BASIC_REGEX_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_BASIC_REGEX_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#ifdef BOOST_XPRESSIVE_DEBUG_TRACKING_POINTER
# include <iostream>
#endif
#include <boost/mpl/bool.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>
#include <boost/xpressive/regex_constants.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>

namespace boost { namespace xpressive
{

///////////////////////////////////////////////////////////////////////////////
// basic_regex
//
/// \brief Class template basic_regex\<\> is a class for holding a compiled regular expression.
template<typename BidiIter>
struct basic_regex
{
    typedef BidiIter iterator_type;
    typedef typename iterator_value<BidiIter>::type char_type;
    typedef std::basic_string<char_type> string_type;
    typedef regex_constants::syntax_option_type flag_type;

    /// \post regex_id()    == 0
    /// \post mark_count()  == 0
    basic_regex()
      : impl_()
    {
    }

    /// \param that The basic_regex object to copy.
    /// \post regex_id()    == that.regex_id()
    /// \post mark_count()  == that.mark_count()
    basic_regex(basic_regex<BidiIter> const &that)
      : impl_(that.impl_)
    {
    }

    /// \param that The basic_regex object to copy.
    /// \post regex_id()    == that.regex_id()
    /// \post mark_count()  == that.mark_count()
    /// \return *this
    basic_regex<BidiIter> &operator =(basic_regex<BidiIter> const &that)
    {
        this->impl_ = that.impl_;
        return *this;
    }

    /// Construct from a static regular expression.
    ///
    /// \param  xpr The static regular expression
    /// \pre    Xpr is the type of a static regular expression.
    /// \post   regex_id()   != 0
    /// \post   mark_count() \>= 0
    template<typename Xpr>
    basic_regex(Xpr const &xpr)
      : impl_()
    {
        this->operator =(xpr);
    }

    /// Construct from a static regular expression.
    ///
    /// \param  xpr The static regular expression.
    /// \pre    Xpr is the type of a static regular expression.
    /// \post   regex_id()   != 0
    /// \post   mark_count() \>= 0
    /// \throw  std::bad_alloc on out of memory
    /// \return *this
    template<typename Xpr>
    basic_regex<BidiIter> &operator =(Xpr const &xpr)
    {
        detail::static_compile(xpr, *this->impl_.get());
        return *this;
    }

    /// Returns the count of capturing sub-expressions in this regular expression
    ///
    std::size_t mark_count() const
    {
        return this->impl_ ? this->impl_->mark_count_ : 0;
    }

    /// Returns a token which uniquely identifies this regular expression.
    ///
    regex_id_type regex_id() const
    {
        return this->impl_ ? this->impl_->xpr_.get() : 0;
    }

    /// Swaps the contents of this basic_regex object with another.
    ///
    /// \param      that The other basic_regex object.
    /// \attention  This is a shallow swap that does not do reference tracking. If you embed
    /// a basic_regex object by reference in another regular expression and then swap its
    /// contents with another basic_regex object, the change will not be visible to the enclosing
    /// regular expression. It is done this way to ensure that swap() cannot throw.
    /// \throw nothrow
    void swap(basic_regex<BidiIter> &that) // throw()
    {
        this->impl_.swap(that.impl_);
    }

    /// Factory method for building a regex object from a string.
    /// Equivalent to regex_compiler\< BidiIter \>().compile(str, flags);
    ///
    /// \param str The std::basic_string containing the regular expression.
    /// \param flags Optional bitmask of type syntax_option_type to control how str is interpreted.
    static basic_regex<BidiIter> compile(string_type const &str, flag_type flags = regex_constants::ECMAScript)
    {
        return regex_compiler<BidiIter>().compile(str, flags);
    }

    // for binding actions to this regex when it is nested statically in another regex
    /// INTERNAL ONLY
    template<typename Action>
    proto::binary_op
    <
        proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
      , proto::unary_op<Action, proto::noop_tag>
      , proto::right_shift_tag
    > const
    operator [](detail::action_matcher<Action> const &action) const
    {
        return proto::noop(*this) >> proto::noop(*static_cast<Action const *>(&action));
    }

    //{{AFX_DEBUG
    #ifdef BOOST_XPRESSIVE_DEBUG_TRACKING_POINTER
    // BUGBUG debug only
    /// INTERNAL ONLY
    friend std::ostream &operator <<(std::ostream &sout, basic_regex<BidiIter> const &rex)
    {
        rex.dump_(sout);
        return sout;
    }
    #endif
    //}}AFX_DEBUG

private:
    friend struct detail::core_access<BidiIter>;

    // Avoid a common programming mistake. Construction from a string is ambiguous. It could mean
    //   sregex rx = sregex::compile(str); // compile the string into a regex
    // or
    //   sregex rx = as_xpr(str);          // treat the string as a literal
    // Since there is no easy way to disambiguate, disallow it and force users to say what they mean
    /// INTERNAL ONLY
    basic_regex(char_type const *);
    /// INTERNAL ONLY
    basic_regex(string_type const &);

    // used from parser, via core_access
    /// INTERNAL ONLY
    explicit basic_regex(detail::regex_impl<BidiIter> const &that)
      : impl_()
    {
        this->impl_.tracking_copy(that);
    }

    /// INTERNAL ONLY
    bool match_(detail::state_type<BidiIter> &state) const
    {
        return this->impl_->xpr_->match(state);
    }

    // Returns true if this basic_regex object does not contain a valid regular expression.
    /// INTERNAL ONLY
    bool invalid_() const
    {
        return !this->impl_ || !this->impl_->xpr_;
    }

    /// INTERNAL ONLY
    void dump_(std::ostream &sout) const;

    // the tracking_ptr manages lazy-init, COW, cycle-breaking, and
    // reference/dependency tracking.
    detail::tracking_ptr<detail::regex_impl<BidiIter> > impl_;
};

//{{AFX_DEBUG
#ifdef BOOST_XPRESSIVE_DEBUG_TRACKING_POINTER
///////////////////////////////////////////////////////////////////////////////
// dump_
/// INTERNAL ONLY
template<typename BidiIter>
inline void basic_regex<BidiIter>::dump_(std::ostream &sout) const
{
    if(!this->impl_)
    {
        sout << "<null> refs={} deps={}";
    }
    else
    {
        sout << *this->impl_;
    }
}
#endif
//}}AFX_DEBUG

///////////////////////////////////////////////////////////////////////////////
// swap
/// \brief      Swaps the contents of two basic_regex objects.
/// \param      left The first basic_regex object.
/// \param      right The second basic_regex object.
/// \attention  This is a shallow swap that does not do reference tracking.
/// If you embed a basic_regex object by reference in another regular expression
/// and then swap its contents with another basic_regex object, the change will
/// not be visible to the enclosing regular expression. It is done this way to
/// ensure that swap() cannot throw.
/// \throw nothrow
template<typename BidiIter>
inline void swap(basic_regex<BidiIter> &left, basic_regex<BidiIter> &right) // throw()
{
    left.swap(right);
}

}} // namespace boost::xpressive

#endif // BOOST_XPRESSIVE_REGEX_HPP_EAN_10_04_2005
