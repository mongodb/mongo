/* boost random/detail/uniform_int_float.hpp header file
 *
 * Copyright Jens Maurer 2000-2001
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id: pass_through_engine.hpp,v 1.5 2004/07/27 03:43:32 dgregor Exp $
 *
 */

#ifndef BOOST_RANDOM_DETAIL_PASS_THROUGH_ENGINE_HPP
#define BOOST_RANDOM_DETAIL_PASS_THROUGH_ENGINE_HPP

#include <boost/config.hpp>
#include <boost/random/detail/ptr_helper.hpp>


namespace boost {
namespace random {
namespace detail {

template<class UniformRandomNumberGenerator>
class pass_through_engine
{
private:
  typedef ptr_helper<UniformRandomNumberGenerator> helper_type;

public:
  typedef typename helper_type::value_type base_type;
  typedef typename base_type::result_type result_type;

  explicit pass_through_engine(UniformRandomNumberGenerator rng)
    // make argument an rvalue to avoid matching Generator& constructor
    : _rng(static_cast<typename helper_type::rvalue_type>(rng))
  { }

  result_type min BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (base().min)(); }
  result_type max BOOST_PREVENT_MACRO_SUBSTITUTION () const { return (base().max)(); }
  base_type& base() { return helper_type::ref(_rng); }
  const base_type& base() const { return helper_type::ref(_rng); }

  result_type operator()() { return base()(); }

private:
  UniformRandomNumberGenerator _rng;
};

#ifndef BOOST_NO_STD_LOCALE

template<class UniformRandomNumberGenerator, class CharT, class Traits>
std::basic_ostream<CharT,Traits>&
operator<<(
    std::basic_ostream<CharT,Traits>& os
    , const pass_through_engine<UniformRandomNumberGenerator>& ud
    )
{
    return os << ud.base();
}

template<class UniformRandomNumberGenerator, class CharT, class Traits>
std::basic_istream<CharT,Traits>&
operator>>(
    std::basic_istream<CharT,Traits>& is
    , const pass_through_engine<UniformRandomNumberGenerator>& ud
    )
{
    return is >> ud.base();
}

#else // no new streams

template<class UniformRandomNumberGenerator>
inline std::ostream&
operator<<(std::ostream& os, 
           const pass_through_engine<UniformRandomNumberGenerator>& ud)
{
    return os << ud.base();
}

template<class UniformRandomNumberGenerator>
inline std::istream&
operator>>(std::istream& is, 
           const pass_through_engine<UniformRandomNumberGenerator>& ud)
{
    return is >> ud.base();
}

#endif

} // namespace detail
} // namespace random
} // namespace boost

#endif // BOOST_RANDOM_DETAIL_PASS_THROUGH_ENGINE_HPP

