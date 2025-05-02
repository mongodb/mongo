/* Copyright 2023 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_SERIALIZE_CONTAINER_HPP
#define BOOST_UNORDERED_DETAIL_SERIALIZE_CONTAINER_HPP

#include <boost/core/serialization.hpp>
#include <boost/throw_exception.hpp>
#include <boost/unordered/detail/archive_constructed.hpp>
#include <boost/unordered/detail/bad_archive_exception.hpp>
#include <boost/unordered/detail/serialization_version.hpp>
#include <cstddef>

namespace boost{
namespace unordered{
namespace detail{

/* serialize_container(ar,x,v) serializes any of the unordered associative
 * containers in Boost.Unordered. Iterator serialization is also supported
 * through the following protocol: 
 *  - At saving time, for each iterator it in [x.begin(),x.end()),
 *    serialization_track(ar,it) is ADL-called to instruct the archive to
 *    track the positions internally pointed to by the iterator via
 *    track_address().
 *  - At loading time, these addresses are mapped to those of the equivalent
 *    reconstructed positions using again serialization_track(ar,it).
 *  - Serializing an iterator reduces to serializing pointers to previously
 *    tracked addresses via serialize_address().
 */

template<typename Iterator>
std::pair<Iterator,bool> adapt_insert_return_type(Iterator it)
{
  return std::pair<Iterator,bool>(it,true);
}

template<typename Iterator>
std::pair<Iterator,bool> adapt_insert_return_type(std::pair<Iterator,bool> p)
{
  return p;
}

template<typename Set,bool IsSaving> struct load_or_save_unordered_set;

template<typename Set> struct load_or_save_unordered_set<Set,true> /* save */
{
  template<typename Archive>
  void operator()(Archive& ar,const Set& x,unsigned int)const
  {
    typedef typename Set::value_type     value_type;
    typedef typename Set::const_iterator const_iterator;

    const std::size_t                       s=x.size();
    const serialization_version<value_type> value_version;

    ar<<core::make_nvp("count",s);
    ar<<core::make_nvp("value_version",value_version);

    for(const_iterator first=x.begin(),last=x.end();first!=last;++first){
      core::save_construct_data_adl(ar,std::addressof(*first),value_version);
      ar<<core::make_nvp("item",*first);
      serialization_track(ar,first);
    }
  }
};

template<typename Set> struct load_or_save_unordered_set<Set,false> /* load */
{
  template<typename Archive>
  void operator()(Archive& ar,Set& x,unsigned int)const
  {
    typedef typename Set::value_type value_type;
    typedef typename Set::iterator   iterator;

    std::size_t                       s;
    serialization_version<value_type> value_version;

    ar>>core::make_nvp("count",s);
    ar>>core::make_nvp("value_version",value_version);

    x.clear();
    x.reserve(s); /* critical so that iterator tracking is stable */

    for(std::size_t n=0;n<s;++n){
      archive_constructed<value_type> value("item",ar,value_version);

      std::pair<iterator,bool> p=adapt_insert_return_type(
        x.insert(std::move(value.get())));
      if(!p.second)throw_exception(bad_archive_exception());
      ar.reset_object_address(
        std::addressof(*p.first),std::addressof(value.get()));
      serialization_track(ar,p.first);
    }
  }
};

template<typename Map,bool IsSaving> struct load_or_save_unordered_map;

template<typename Map> struct load_or_save_unordered_map<Map,true> /* save */
{
  template<typename Archive>
  void operator()(Archive& ar,const Map& x,unsigned int)const
  {
    typedef typename std::remove_const<
      typename Map::key_type>::type       key_type;
    typedef typename std::remove_const<
      typename Map::mapped_type>::type    mapped_type;
    typedef typename Map::const_iterator  const_iterator;

    const std::size_t                        s=x.size();
    const serialization_version<key_type>    key_version;
    const serialization_version<mapped_type> mapped_version;

    ar<<core::make_nvp("count",s);
    ar<<core::make_nvp("key_version",key_version);
    ar<<core::make_nvp("mapped_version",mapped_version);

    for(const_iterator first=x.begin(),last=x.end();first!=last;++first){
      /* To remain lib-independent from Boost.Serialization and not rely on
       * the user having included the serialization code for std::pair
       * (boost/serialization/utility.hpp), we serialize the key and the
       * mapped value separately.
       */

      core::save_construct_data_adl(
        ar,std::addressof(first->first),key_version);
      ar<<core::make_nvp("key",first->first);
      core::save_construct_data_adl(
        ar,std::addressof(first->second),mapped_version);
      ar<<core::make_nvp("mapped",first->second);
      serialization_track(ar,first);
    }
  }
};

template<typename Map> struct load_or_save_unordered_map<Map,false> /* load */
{
  template<typename Archive>
  void operator()(Archive& ar,Map& x,unsigned int)const
  {
    typedef typename std::remove_const<
      typename Map::key_type>::type       key_type;
    typedef typename std::remove_const<
      typename Map::mapped_type>::type    mapped_type;
    typedef typename Map::iterator        iterator;

    std::size_t                        s;
    serialization_version<key_type>    key_version;
    serialization_version<mapped_type> mapped_version;

    ar>>core::make_nvp("count",s);
    ar>>core::make_nvp("key_version",key_version);
    ar>>core::make_nvp("mapped_version",mapped_version);

    x.clear();
    x.reserve(s); /* critical so that iterator tracking is stable */

    for(std::size_t n=0;n<s;++n){
      archive_constructed<key_type>    key("key",ar,key_version);
      archive_constructed<mapped_type> mapped("mapped",ar,mapped_version);

      std::pair<iterator,bool> p=adapt_insert_return_type(
        x.emplace(std::move(key.get()),std::move(mapped.get())));
      if(!p.second)throw_exception(bad_archive_exception());
      ar.reset_object_address(
        std::addressof(p.first->first),std::addressof(key.get()));
      ar.reset_object_address(
        std::addressof(p.first->second),std::addressof(mapped.get()));
      serialization_track(ar,p.first);
    }
  }
};

template<typename Container,bool IsSet,bool IsSaving>
struct load_or_save_container;
  
template<typename Set,bool IsSaving>
struct load_or_save_container<Set,true,IsSaving>:
  load_or_save_unordered_set<Set,IsSaving>{};

template<typename Map,bool IsSaving>
struct load_or_save_container<Map,false,IsSaving>:
  load_or_save_unordered_map<Map,IsSaving>{};

template<typename Archive,typename Container>
void serialize_container(Archive& ar,Container& x,unsigned int version)
{
  load_or_save_container<
    Container,
    std::is_same<
      typename Container::key_type,typename Container::value_type>::value,
    Archive::is_saving::value>()(ar,x,version);
}

} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
