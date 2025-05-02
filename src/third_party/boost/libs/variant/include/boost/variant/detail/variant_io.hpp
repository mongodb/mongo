//-----------------------------------------------------------------------------
// boost variant/detail/variant_io.hpp header file
// See http://www.boost.org for updates, documentation, and revision history.
//-----------------------------------------------------------------------------
//
// Copyright (c) 2002-2003
// Eric Friedman, Itay Maman
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_VARIANT_DETAIL_VARIANT_IO_HPP
#define BOOST_VARIANT_DETAIL_VARIANT_IO_HPP

#include <iosfwd> // for std::basic_ostream forward declare

#include <boost/variant/variant_fwd.hpp>
#include <boost/variant/static_visitor.hpp>

namespace boost {

///////////////////////////////////////////////////////////////////////////////
// function template operator<<
//
// Outputs the content of the given variant to the given ostream.
//

// forward declare (allows output of embedded variant< variant< ... >, ... >)
template <class CharT, class Trait, typename... U>
inline std::basic_ostream<CharT, Trait>& operator<<(
      std::basic_ostream<CharT, Trait>& out, const variant<U...>& rhs
    );

namespace detail { namespace variant {

template <typename OStream>
class printer
    : public boost::static_visitor<>
{
private: // representation

    OStream& out_;

public: // structors

    explicit printer(OStream& out)
        : out_( out )
    {
    }

public: // visitor interface

    template <typename T>
    void operator()(const T& operand) const
    {
        out_ << operand;
    }

private:
    printer& operator=(const printer&);

};

}} // namespace detail::variant

template <class CharT, class Trait, typename... U>
inline std::basic_ostream<CharT, Trait>& operator<<(
      std::basic_ostream<CharT, Trait>& out, const variant<U...>& rhs
    )
{
    detail::variant::printer<
          std::basic_ostream<CharT, Trait>
        > visitor(out);

    rhs.apply_visitor(visitor);

    return out;
}

} // namespace boost

#endif // BOOST_VARIANT_DETAIL_VARIANT_IO_HPP
