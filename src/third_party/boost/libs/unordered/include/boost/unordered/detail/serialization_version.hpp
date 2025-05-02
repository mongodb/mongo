/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_SERIALIZATION_VERSION_HPP
#define BOOST_UNORDERED_DETAIL_SERIALIZATION_VERSION_HPP

#include <boost/config.hpp>
#include <boost/core/serialization.hpp>

namespace boost{
namespace unordered{
namespace detail{

/* boost::serialization::load_construct_adl(ar,t,version) requires user code
 * to pass the serialization version for t, when this information is really
 * stored in the archive. serialization_version<T> circumvents this design
 * error by acting as a regular serializable type with the same serialization
 * version as T; loading/saving serialization_version<T> does nothing with
 * the archive data itself but captures the stored serialization version
 * at load() time.
 */

template<typename T>
struct serialization_version
{
  serialization_version():
    value(boost::serialization::version<serialization_version>::value){}

  serialization_version& operator=(unsigned int x){value=x;return *this;};

  operator unsigned int()const{return value;}

private:
  friend class boost::serialization::access;

  template<class Archive>
  void serialize(Archive& ar,unsigned int version)
  {
    core::split_member(ar,*this,version);
  }

  template<class Archive>
  void save(Archive&,unsigned int)const{}

  template<class Archive>
  void load(Archive&,unsigned int version)
  {
    this->value=version;
  }

  unsigned int value;
};

} /* namespace detail */
} /* namespace unordered */

namespace serialization{

template<typename T>
struct version<boost::unordered::detail::serialization_version<T> >
{
  BOOST_STATIC_CONSTANT(int,value=version<T>::value);
};

} /* namespace serialization */

} /* namespace boost */

#endif
