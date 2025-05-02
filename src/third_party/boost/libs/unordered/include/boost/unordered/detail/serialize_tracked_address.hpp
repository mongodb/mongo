/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_SERIALIZE_TRACKED_ADDRESS_HPP
#define BOOST_UNORDERED_DETAIL_SERIALIZE_TRACKED_ADDRESS_HPP

#include <boost/unordered/detail/bad_archive_exception.hpp>

#include <boost/core/pointer_traits.hpp>
#include <boost/core/serialization.hpp>
#include <boost/throw_exception.hpp>

#include <type_traits>

namespace boost{
namespace unordered{
namespace detail{

/* Tracked address serialization to support iterator serialization as described
 * in serialize_container.hpp. The underlying technique is to reinterpret_cast
 * T pointers to serialization_tracker<T> pointers, which, when dereferenced
 * and serialized, do not emit any serialization payload to the
 * archive, but activate object tracking on the relevant addresses for later
 * use with serialize_tracked_address().
 */

template<typename T>
struct serialization_tracker
{
  /* An attempt to construct a serialization_tracker means a stray address
   * in the archive, that is, one without a previously tracked address.
   */
  serialization_tracker(){throw_exception(bad_archive_exception());}

  template<typename Archive>
  void serialize(Archive&,unsigned int){} /* no data emitted */
};

template<typename Archive,typename Ptr>
void track_address(Archive& ar,Ptr p)
{
  typedef typename boost::pointer_traits<Ptr> ptr_traits;
  typedef typename std::remove_const<
    typename ptr_traits::element_type>::type  element_type;

  if(p){
    ar&core::make_nvp(
      "address",
      *reinterpret_cast<serialization_tracker<element_type>*>(
        const_cast<element_type*>(
          boost::to_address(p))));
  }
}

template<typename Archive,typename Ptr>
void serialize_tracked_address(Archive& ar,Ptr& p,std::true_type /* save */)
{
  typedef typename boost::pointer_traits<Ptr> ptr_traits;
  typedef typename std::remove_const<
    typename ptr_traits::element_type>::type  element_type;
  typedef serialization_tracker<element_type> tracker;

  tracker* pt=
    const_cast<tracker*>(
      reinterpret_cast<const tracker*>(
        const_cast<const element_type*>(
          boost::to_address(p))));
  ar<<core::make_nvp("pointer",pt);
}

template<typename Archive,typename Ptr>
void serialize_tracked_address(Archive& ar,Ptr& p,std::false_type /* load */)
{
  typedef typename boost::pointer_traits<Ptr> ptr_traits;
  typedef typename std::remove_const<
    typename ptr_traits::element_type>::type  element_type;
  typedef serialization_tracker<element_type> tracker;

  tracker* pt;
  ar>>core::make_nvp("pointer",pt);
  element_type* pn=const_cast<element_type*>(
    reinterpret_cast<const element_type*>(
      const_cast<const tracker*>(pt)));
  p=pn?ptr_traits::pointer_to(*pn):0;
}

template<typename Archive,typename Ptr>
void serialize_tracked_address(Archive& ar,Ptr& p)
{
  serialize_tracked_address(
    ar,p,
    std::integral_constant<bool,Archive::is_saving::value>());
}

} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
