//  (C) Copyright Jeremy Siek 2006
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROPERTY_SERIALIZE_HPP
#define BOOST_PROPERTY_SERIALIZE_HPP

#include <boost/pending/property.hpp>

#include <boost/serialization/base_object.hpp>
#include <boost/serialization/nvp.hpp>

namespace boost {
  template<class Archive>
  inline void serialize(Archive&, no_property&, const unsigned int) { }

  template<class Archive, class Tag, class T, class Base>
  void 
  serialize(Archive& ar, property<Tag, T, Base>& prop, 
            const unsigned int version) 
  {
    ar & serialization::make_nvp( "property_base" , boost::serialization::base_object<Base>(prop) );
    ar & serialization::make_nvp( "property_value" , prop.m_value );
  }
} // end namespace boost

#endif // BOOST_PROPERTY_SERIALIZE_HPP
