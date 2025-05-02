/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_SERIALIZE_FCA_CONTAINER_HPP
#define BOOST_UNORDERED_DETAIL_SERIALIZE_FCA_CONTAINER_HPP

#include <boost/unordered/detail/serialize_container.hpp>

#if defined(BOOST_UNORDERED_ENABLE_SERIALIZATION_COMPATIBILITY_V0)

#define BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER \
  <boost/serialization/archive_input_unordered_map.hpp>
#include BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#undef BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#define BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER \
  <boost/serialization/archive_input_unordered_set.hpp>
#include BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#undef BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#define BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER \
  <boost/serialization/unordered_collections_load_imp.hpp>
#include BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#undef BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#define BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER \
  <boost/serialization/utility.hpp>
#include BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER
#undef BOOST_UNORDERED_BLOCK_BOOSTDEP_HEADER

#include <boost/unordered/unordered_map_fwd.hpp>
#include <boost/unordered/unordered_set_fwd.hpp>

#else

#include <boost/throw_exception.hpp>
#include <stdexcept>

#endif

namespace boost{
namespace unordered{
namespace detail{

/* Support for boost::unordered_[multi](map|set) loading from legacy archives.
 * Until Boost 1.84, serialization of these containers was provided from
 * Boost.Serialization via boost/serialization/boost_unordered_(map|set).hpp,
 * from that release on support is native in Boost.Unordered. To enable legacy
 * archive loading, BOOST_UNORDERED_ENABLE_SERIALIZATION_COMPATIBILITY_V0
 * must be defined (it implies header dependency from Boost.Serialization).
 */

#if defined(BOOST_UNORDERED_ENABLE_SERIALIZATION_COMPATIBILITY_V0)

template<typename Archive,typename Container>
struct archive_input;

template<
  typename Archive,typename K,typename T,typename H,typename P,typename A
>
struct archive_input<Archive,boost::unordered_map<K,T,H,P,A> >:
  boost::serialization::stl::archive_input_unordered_map<
    Archive,
    boost::unordered_map<K,T,H,P,A>
  >
{};

template<
  typename Archive,typename K,typename T,typename H,typename P,typename A
>
struct archive_input<Archive,boost::unordered_multimap<K,T,H,P,A> >:
  boost::serialization::stl::archive_input_unordered_multimap<
    Archive,
    boost::unordered_multimap<K,T,H,P,A>
  >
{};

template<
  typename Archive,typename K,typename H,typename P,typename A
>
struct archive_input<Archive,boost::unordered_set<K,H,P,A> >:
  boost::serialization::stl::archive_input_unordered_set<
    Archive,
    boost::unordered_set<K,H,P,A>
  >
{};

template<
  typename Archive,typename K,typename H,typename P,typename A
>
struct archive_input<Archive,boost::unordered_multiset<K,H,P,A> >:
  boost::serialization::stl::archive_input_unordered_multiset<
    Archive,
    boost::unordered_multiset<K,H,P,A>
  >
{};

#else

struct legacy_archive_exception:std::runtime_error
{
  legacy_archive_exception():std::runtime_error(
    "Legacy archive detected, define "
    "BOOST_UNORDERED_ENABLE_SERIALIZATION_COMPATIBILITY_V0 to load"){}
};

#endif

template<typename Container,bool IsSaving>
struct load_or_save_fca_container;

template<typename Container>
struct load_or_save_fca_container<Container,true> /* save */
{
  template<typename Archive>
  void operator()(Archive& ar,Container& x,unsigned int version)const
  {
    serialize_container(ar,x,version);
  }
};

template<typename Container>
struct load_or_save_fca_container<Container,false> /* load */
{
  template<typename Archive>
  void operator()(Archive& ar,Container& x,unsigned int version)const
  {
    if(version==0){
#if defined(BOOST_UNORDERED_ENABLE_SERIALIZATION_COMPATIBILITY_V0)
      boost::serialization::stl::load_unordered_collection<
        Archive,Container,archive_input<Archive,Container>
      >(ar,x);
#else
      throw_exception(legacy_archive_exception());
#endif
    }
    else{
      serialize_container(ar,x,version);
    }
  }
};

template<typename Archive,typename Container>
void serialize_fca_container(Archive& ar,Container& x,unsigned int version)
{
  load_or_save_fca_container<Container,Archive::is_saving::value>()(
    ar,x,version);
}

} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
