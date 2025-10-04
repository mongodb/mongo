/* Fast open-addressing hash table.
 *
 * Copyright 2022-2024 Joaquin M Lopez Munoz.
 * Copyright 2023 Christian Mazakas.
 * Copyright 2024 Braden Ganetsky.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_TABLE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_TABLE_HPP

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <boost/core/serialization.hpp>
#include <boost/unordered/detail/foa/core.hpp>
#include <boost/unordered/detail/serialize_tracked_address.hpp>
#include <boost/unordered/detail/type_traits.hpp>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

/* use plain integrals for group metadata storage */

template<typename Integral>
struct plain_integral
{
  operator Integral()const{return n;}
  void operator=(Integral m){n=m;}

#if BOOST_WORKAROUND(BOOST_GCC,>=50000 && BOOST_GCC<60000)
  void operator|=(Integral m){n=static_cast<Integral>(n|m);}
  void operator&=(Integral m){n=static_cast<Integral>(n&m);}
#else
  void operator|=(Integral m){n|=m;}
  void operator&=(Integral m){n&=m;}
#endif

  Integral n;
};

struct plain_size_control
{
  std::size_t ml;
  std::size_t size;
};

template<typename,typename,typename,typename>
class table;

/* table_iterator keeps two pointers:
 * 
 *   - A pointer p to the element slot.
 *   - A pointer pc to the n-th byte of the associated group metadata, where n
 *     is the position of the element in the group.
 *
 * A simpler solution would have been to keep a pointer p to the element, a
 * pointer pg to the group, and the position n, but that would increase
 * sizeof(table_iterator) by 4/8 bytes. In order to make this compact
 * representation feasible, it is required that group objects are aligned
 * to their size, so that we can recover pg and n as
 * 
 *   - n = pc%sizeof(group)
 *   - pg = pc-n
 * 
 * (for explanatory purposes pg and pc are treated above as if they were memory
 * addresses rather than pointers).
 * 
 * p = nullptr is conventionally used to mark end() iterators.
 */

/* internal conversion from const_iterator to iterator */
struct const_iterator_cast_tag{}; 

template<typename TypePolicy,typename GroupPtr,bool Const>
class table_iterator
{
  using group_pointer_traits=boost::pointer_traits<GroupPtr>;
  using type_policy=TypePolicy;
  using table_element_type=typename type_policy::element_type;
  using group_type=typename group_pointer_traits::element_type;
  using table_element_pointer=
    typename group_pointer_traits::template rebind<table_element_type>;
  using char_pointer=
    typename group_pointer_traits::template rebind<unsigned char>;
  static constexpr auto N=group_type::N;
  static constexpr auto regular_layout=group_type::regular_layout;

public:
  using difference_type=std::ptrdiff_t;
  using value_type=typename type_policy::value_type;
  using pointer=
    typename std::conditional<Const,value_type const*,value_type*>::type;
  using reference=
    typename std::conditional<Const,value_type const&,value_type&>::type;
  using iterator_category=std::forward_iterator_tag;
  using element_type=
    typename std::conditional<Const,value_type const,value_type>::type;

  table_iterator():pc_{nullptr},p_{nullptr}{};
  template<bool Const2,typename std::enable_if<!Const2>::type* =nullptr>
  table_iterator(const table_iterator<TypePolicy,GroupPtr,Const2>& x):
    pc_{x.pc_},p_{x.p_}{}
  table_iterator(
    const_iterator_cast_tag, const table_iterator<TypePolicy,GroupPtr,true>& x):
    pc_{x.pc_},p_{x.p_}{}

  inline reference operator*()const noexcept
    {return type_policy::value_from(*p());}
  inline pointer operator->()const noexcept
    {return std::addressof(type_policy::value_from(*p()));}
  inline table_iterator& operator++()noexcept{increment();return *this;}
  inline table_iterator operator++(int)noexcept
    {auto x=*this;increment();return x;}
  friend inline bool operator==(
    const table_iterator& x,const table_iterator& y)
    {return x.p()==y.p();}
  friend inline bool operator!=(
    const table_iterator& x,const table_iterator& y)
    {return !(x==y);}

private:
  template<typename,typename,bool> friend class table_iterator;
  template<typename> friend class table_erase_return_type;
  template<typename,typename,typename,typename> friend class table;

  table_iterator(group_type* pg,std::size_t n,const table_element_type* ptet):
    pc_{to_pointer<char_pointer>(
      reinterpret_cast<unsigned char*>(const_cast<group_type*>(pg))+n)},
    p_{to_pointer<table_element_pointer>(const_cast<table_element_type*>(ptet))}
  {}

  unsigned char* pc()const noexcept{return boost::to_address(pc_);}
  table_element_type* p()const noexcept{return boost::to_address(p_);}

  inline void increment()noexcept
  {
    BOOST_ASSERT(p()!=nullptr);
    increment(std::integral_constant<bool,regular_layout>{});
  }

  inline void increment(std::true_type /* regular layout */)noexcept
  {
    using diff_type=
      typename boost::pointer_traits<char_pointer>::difference_type;

    for(;;){
      ++p_;
      if(reinterpret_cast<uintptr_t>(pc())%sizeof(group_type)==N-1){
        pc_+=static_cast<diff_type>(sizeof(group_type)-(N-1));
        break;
      }
      ++pc_;
      if(!group_type::is_occupied(pc()))continue;
      if(BOOST_UNLIKELY(group_type::is_sentinel(pc())))p_=nullptr;
      return;
    }

    for(;;){
      int mask=reinterpret_cast<group_type*>(pc())->match_occupied();
      if(mask!=0){
        auto n=unchecked_countr_zero(mask);
        if(BOOST_UNLIKELY(reinterpret_cast<group_type*>(pc())->is_sentinel(n))){
          p_=nullptr;
        }
        else{
          pc_+=static_cast<diff_type>(n);
          p_+=static_cast<diff_type>(n);
        }
        return;
      }
      pc_+=static_cast<diff_type>(sizeof(group_type));
      p_+=static_cast<diff_type>(N);
    }
  }

  inline void increment(std::false_type /* interleaved */)noexcept
  {
    using diff_type=
      typename boost::pointer_traits<char_pointer>::difference_type;

    std::size_t n0=reinterpret_cast<uintptr_t>(pc())%sizeof(group_type);
    pc_-=static_cast<diff_type>(n0);

    int mask=(
      reinterpret_cast<group_type*>(pc())->match_occupied()>>(n0+1))<<(n0+1);
    if(!mask){
      do{
        pc_+=sizeof(group_type);
        p_+=N;
      }
      while((mask=reinterpret_cast<group_type*>(pc())->match_occupied())==0);
    }

    auto n=unchecked_countr_zero(mask);
    if(BOOST_UNLIKELY(reinterpret_cast<group_type*>(pc())->is_sentinel(n))){
      p_=nullptr;
    }
    else{
      pc_+=static_cast<diff_type>(n);
      p_-=static_cast<diff_type>(n0);
      p_+=static_cast<diff_type>(n);
    }
  }

  template<typename Archive>
  friend void serialization_track(Archive& ar,const table_iterator& x)
  {
    if(x.p()){
      track_address(ar,x.pc_);
      track_address(ar,x.p_);
    }
  }

  friend class boost::serialization::access;

  template<typename Archive>
  void serialize(Archive& ar,unsigned int)
  {
    if(!p())pc_=nullptr;
    serialize_tracked_address(ar,pc_);
    serialize_tracked_address(ar,p_);
  }

  char_pointer          pc_=nullptr;
  table_element_pointer  p_=nullptr;
};

/* Returned by table::erase([const_]iterator) to avoid iterator increment
 * if discarded.
 */

template<typename Iterator>
class table_erase_return_type; 

template<typename TypePolicy,typename GroupPtr,bool Const>
class table_erase_return_type<table_iterator<TypePolicy,GroupPtr,Const>>
{
  using iterator=table_iterator<TypePolicy,GroupPtr,Const>;
  using const_iterator=table_iterator<TypePolicy,GroupPtr,true>;

public:
  /* can't delete it because VS in pre-C++17 mode needs to see it for RVO */
  table_erase_return_type(const table_erase_return_type&);

  operator iterator()const noexcept
  {
    auto it=pos;
    it.increment(); /* valid even if *it was erased */
    return iterator(const_iterator_cast_tag{},it);
  }

  template<
    bool dependent_value=false,
    typename std::enable_if<!Const||dependent_value>::type* =nullptr
  >
  operator const_iterator()const noexcept{return this->operator iterator();}

private:
  template<typename,typename,typename,typename> friend class table;

  table_erase_return_type(const_iterator pos_):pos{pos_}{}
  table_erase_return_type& operator=(const table_erase_return_type&)=delete;

  const_iterator pos;
};

/* foa::table interface departs in a number of ways from that of C++ unordered
 * associative containers because it's not for end-user consumption
 * (boost::unordered_(flat|node)_(map|set) wrappers complete it as
 * appropriate).
 *
 * The table supports two main modes of operation: flat and node-based. In the
 * flat case, buckets directly store elements. For node-based, buckets store
 * pointers to individually heap-allocated elements.
 *
 * For both flat and node-based:
 *
 *   - begin() is not O(1).
 *   - No bucket API.
 *   - Load factor is fixed and can't be set by the user.
 * 
 * For flat only:
 *
 *   - value_type must be moveable.
 *   - Pointer stability is not kept under rehashing.
 *   - No extract API.
 *
 * try_emplace, erase and find support heterogeneous lookup by default,
 * that is, without checking for any ::is_transparent typedefs --the
 * checking is done by boost::unordered_(flat|node)_(map|set).
 */

template<typename,typename,typename,typename>
class concurrent_table; /* concurrent/non-concurrent interop */

template <typename TypePolicy,typename Hash,typename Pred,typename Allocator>
using table_core_impl=
  table_core<TypePolicy,group15<plain_integral>,table_arrays,
  plain_size_control,Hash,Pred,Allocator>;

#include <boost/unordered/detail/foa/ignore_wshadow.hpp>

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4714) /* marked as __forceinline not inlined */
#endif

template<typename TypePolicy,typename Hash,typename Pred,typename Allocator>
class table:table_core_impl<TypePolicy,Hash,Pred,Allocator>
{
  using super=table_core_impl<TypePolicy,Hash,Pred,Allocator>;
  using type_policy=typename super::type_policy;
  using group_type=typename super::group_type;
  using super::N;
  using prober=typename super::prober;
  using arrays_type=typename super::arrays_type;
  using size_ctrl_type=typename super::size_ctrl_type;
  using locator=typename super::locator;
  using compatible_concurrent_table=
    concurrent_table<TypePolicy,Hash,Pred,Allocator>;
  using group_type_pointer=typename boost::pointer_traits<
    typename boost::allocator_pointer<Allocator>::type
  >::template rebind<group_type>;
  friend compatible_concurrent_table;

public:
  using key_type=typename super::key_type;
  using init_type=typename super::init_type;
  using value_type=typename super::value_type;
  using element_type=typename super::element_type;

private:
  static constexpr bool has_mutable_iterator=
    !std::is_same<key_type,value_type>::value;
public:
  using hasher=typename super::hasher;
  using key_equal=typename super::key_equal;
  using allocator_type=typename super::allocator_type;
  using pointer=typename super::pointer;
  using const_pointer=typename super::const_pointer;
  using reference=typename super::reference;
  using const_reference=typename super::const_reference;
  using size_type=typename super::size_type;
  using difference_type=typename super::difference_type;
  using const_iterator=table_iterator<type_policy,group_type_pointer,true>;
  using iterator=typename std::conditional<
    has_mutable_iterator,
    table_iterator<type_policy,group_type_pointer,false>,
    const_iterator>::type;
  using erase_return_type=table_erase_return_type<iterator>;

#if defined(BOOST_UNORDERED_ENABLE_STATS)
  using stats=typename super::stats;
#endif

  table(
    std::size_t n=default_bucket_count,const Hash& h_=Hash(),
    const Pred& pred_=Pred(),const Allocator& al_=Allocator()):
    super{n,h_,pred_,al_}
    {}

  table(const table& x)=default;
  table(table&& x)=default;
  table(const table& x,const Allocator& al_):super{x,al_}{}
  table(table&& x,const Allocator& al_):super{std::move(x),al_}{}
  table(compatible_concurrent_table&& x):
    table(std::move(x),x.exclusive_access()){}
  ~table()=default;

  table& operator=(const table& x)=default;
  table& operator=(table&& x)=default;

  using super::get_allocator;

  iterator begin()noexcept
  {
    iterator it{this->arrays.groups(),0,this->arrays.elements()};
    if(this->arrays.elements()&&
       !(this->arrays.groups()[0].match_occupied()&0x1))++it;
    return it;
  }

  const_iterator begin()const noexcept
                   {return const_cast<table*>(this)->begin();}
  iterator       end()noexcept{return {};}
  const_iterator end()const noexcept{return const_cast<table*>(this)->end();}
  const_iterator cbegin()const noexcept{return begin();}
  const_iterator cend()const noexcept{return end();}

  using super::empty;
  using super::size;
  using super::max_size;

  template<typename... Args>
  BOOST_FORCEINLINE std::pair<iterator,bool> emplace(Args&&... args)
  {
    alloc_cted_insert_type<type_policy,Allocator,Args...> x(
      this->al(),std::forward<Args>(args)...);
    return emplace_impl(type_policy::move(x.value()));
  }

  /* Optimization for value_type and init_type, to avoid constructing twice */
  template <typename T>
  BOOST_FORCEINLINE typename std::enable_if<
    detail::is_similar_to_any<T, value_type, init_type>::value,
    std::pair<iterator, bool> >::type
  emplace(T&& x)
  {
    return emplace_impl(std::forward<T>(x));
  }

  /* Optimizations for maps for (k,v) to avoid eagerly constructing value */
  template <typename K, typename V>
  BOOST_FORCEINLINE
    typename std::enable_if<is_emplace_kv_able<table, K>::value,
      std::pair<iterator, bool> >::type
    emplace(K&& k, V&& v)
  {
    alloc_cted_or_fwded_key_type<type_policy, Allocator, K&&> x(
      this->al(), std::forward<K>(k));
    return emplace_impl(
      try_emplace_args_t{}, x.move_or_fwd(), std::forward<V>(v));
  }

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE std::pair<iterator,bool> try_emplace(
    Key&& x,Args&&... args)
  {
    return emplace_impl(
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  BOOST_FORCEINLINE std::pair<iterator,bool>
  insert(const init_type& x){return emplace_impl(x);}

  BOOST_FORCEINLINE std::pair<iterator,bool>
  insert(init_type&& x){return emplace_impl(std::move(x));}

  /* template<typename=void> tilts call ambiguities in favor of init_type */

  template<typename=void>
  BOOST_FORCEINLINE std::pair<iterator,bool>
  insert(const value_type& x){return emplace_impl(x);}

  template<typename=void>
  BOOST_FORCEINLINE std::pair<iterator,bool>
  insert(value_type&& x){return emplace_impl(std::move(x));}

  template<typename T=element_type>
  BOOST_FORCEINLINE
  typename std::enable_if<
    !std::is_same<T,value_type>::value,
    std::pair<iterator,bool>
  >::type
  insert(element_type&& x){return emplace_impl(std::move(x));}

  template<
    bool dependent_value=false,
    typename std::enable_if<
      has_mutable_iterator||dependent_value>::type* =nullptr
  >
  erase_return_type erase(iterator pos)noexcept
  {return erase(const_iterator(pos));}

  BOOST_FORCEINLINE
  erase_return_type erase(const_iterator pos)noexcept
  {
    super::erase(pos.pc(),pos.p());
    return {pos};
  }

  template<typename Key>
  BOOST_FORCEINLINE
  auto erase(Key&& x) -> typename std::enable_if<
    !std::is_convertible<Key,iterator>::value&&
    !std::is_convertible<Key,const_iterator>::value, std::size_t>::type
  {
    auto it=find(x);
    if(it!=end()){
      erase(it);
      return 1;
    }
    else return 0;
  }

  void swap(table& x)
    noexcept(noexcept(std::declval<super&>().swap(std::declval<super&>())))
  {
    super::swap(x);
  }

  using super::clear;

  element_type extract(const_iterator pos)
  {
    BOOST_ASSERT(pos!=end());
    erase_on_exit e{*this,pos};
    (void)e;
    return std::move(*pos.p());
  }

  // TODO: should we accept different allocator too?
  template<typename Hash2,typename Pred2>
  void merge(table<TypePolicy,Hash2,Pred2,Allocator>& x)
  {
    x.for_all_elements([&,this](group_type* pg,unsigned int n,element_type* p){
      erase_on_exit e{x,{pg,n,p}};
      if(!emplace_impl(type_policy::move(*p)).second)e.rollback();
    });
  }

  template<typename Hash2,typename Pred2>
  void merge(table<TypePolicy,Hash2,Pred2,Allocator>&& x){merge(x);}

  using super::hash_function;
  using super::key_eq;

  template<typename Key>
  BOOST_FORCEINLINE iterator find(const Key& x)
  {
    return make_iterator(super::find(x));
  }

  template<typename Key>
  BOOST_FORCEINLINE const_iterator find(const Key& x)const
  {
    return const_cast<table*>(this)->find(x);
  }

  using super::capacity;
  using super::load_factor;
  using super::max_load_factor;
  using super::max_load;
  using super::rehash;
  using super::reserve;

#if defined(BOOST_UNORDERED_ENABLE_STATS)
  using super::get_stats;
  using super::reset_stats;
#endif

  template<typename Predicate>
  friend std::size_t erase_if(table& x,Predicate& pr)
  {
    using value_reference=typename std::conditional<
      std::is_same<key_type,value_type>::value,
      const_reference,
      reference
    >::type;

    std::size_t s=x.size();
    x.for_all_elements(
      [&](group_type* pg,unsigned int n,element_type* p){
        if(pr(const_cast<value_reference>(type_policy::value_from(*p)))){
          x.super::erase(pg,n,p);
        }
      });
    return std::size_t(s-x.size());
  }

  friend bool operator==(const table& x,const table& y)
  {
    return static_cast<const super&>(x)==static_cast<const super&>(y);
  }

  friend bool operator!=(const table& x,const table& y){return !(x==y);}

private:
  template<typename ArraysType>
  table(compatible_concurrent_table&& x,arrays_holder<ArraysType,Allocator>&& ah):
    super{
      std::move(x.h()),std::move(x.pred()),std::move(x.al()),
      [&x]{return arrays_type{
        x.arrays.groups_size_index,x.arrays.groups_size_mask,
        to_pointer<group_type_pointer>(
          reinterpret_cast<group_type*>(x.arrays.groups())),
        x.arrays.elements_};},
      size_ctrl_type{x.size_ctrl.ml,x.size_ctrl.size}}
  {
    compatible_concurrent_table::arrays_type::delete_group_access(x.al(),x.arrays);
    x.arrays=ah.release();
    x.size_ctrl.ml=x.initial_max_load();
    x.size_ctrl.size=0;
    BOOST_UNORDERED_SWAP_STATS(this->cstats,x.cstats);
  }

  template<typename ExclusiveLockGuard>
  table(compatible_concurrent_table&& x,ExclusiveLockGuard):
    table(std::move(x),x.make_empty_arrays())
  {}

  struct erase_on_exit
  {
    erase_on_exit(table& x_,const_iterator it_):x(x_),it(it_){}
    ~erase_on_exit(){if(!rollback_)x.erase(it);}

    void rollback(){rollback_=true;}

    table&         x;
    const_iterator it;
    bool           rollback_=false;
  };

  static inline iterator make_iterator(const locator& l)noexcept
  {
    return {l.pg,l.n,l.p};
  }

  template<typename... Args>
  BOOST_FORCEINLINE std::pair<iterator,bool> emplace_impl(Args&&... args)
  {
    const auto &k=this->key_from(std::forward<Args>(args)...);
    auto        hash=this->hash_for(k);
    auto        pos0=this->position_for(hash);
    auto        loc=super::find(k,pos0,hash);

    if(loc){
      return {make_iterator(loc),false};
    }
    if(BOOST_LIKELY(this->size_ctrl.size<this->size_ctrl.ml)){
      return {
        make_iterator(
          this->unchecked_emplace_at(pos0,hash,std::forward<Args>(args)...)),
        true
      };  
    }
    else{
      return {
        make_iterator(
          this->unchecked_emplace_with_rehash(
            hash,std::forward<Args>(args)...)),
        true
      };  
    }
  }
};

#if defined(BOOST_MSVC)
#pragma warning(pop) /* C4714 */
#endif

#include <boost/unordered/detail/foa/restore_wshadow.hpp>

} /* namespace foa */
} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
