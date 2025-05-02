/* Fast open-addressing concurrent hash table.
 *
 * Copyright 2023-2024 Joaquin M Lopez Munoz.
 * Copyright 2024 Braden Ganetsky.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_CONCURRENT_TABLE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_CONCURRENT_TABLE_HPP

#include <atomic>
#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/core/no_exceptions_support.hpp>
#include <boost/core/serialization.hpp>
#include <boost/cstdint.hpp>
#include <boost/mp11/tuple.hpp>
#include <boost/throw_exception.hpp>
#include <boost/unordered/detail/archive_constructed.hpp>
#include <boost/unordered/detail/bad_archive_exception.hpp>
#include <boost/unordered/detail/foa/core.hpp>
#include <boost/unordered/detail/foa/reentrancy_check.hpp>
#include <boost/unordered/detail/foa/rw_spinlock.hpp>
#include <boost/unordered/detail/foa/tuple_rotate_right.hpp>
#include <boost/unordered/detail/serialization_version.hpp>
#include <boost/unordered/detail/static_assert.hpp>
#include <boost/unordered/detail/type_traits.hpp>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <tuple>
#include <utility>

#if !defined(BOOST_UNORDERED_DISABLE_PARALLEL_ALGORITHMS)
#if defined(BOOST_UNORDERED_ENABLE_PARALLEL_ALGORITHMS)|| \
    !defined(BOOST_NO_CXX17_HDR_EXECUTION)
#define BOOST_UNORDERED_PARALLEL_ALGORITHMS
#endif
#endif

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
#include <algorithm>
#include <execution>
#endif

namespace boost{
namespace unordered{
namespace detail{

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)

template<typename ExecutionPolicy>
using is_execution_policy=std::is_execution_policy<
  typename std::remove_cv<
    typename std::remove_reference<ExecutionPolicy>::type
  >::type
>;

#else

template<typename ExecutionPolicy>
using is_execution_policy=std::false_type;

#endif

namespace foa{

static constexpr std::size_t cacheline_size=64;

template<typename T,std::size_t N>
class cache_aligned_array
{
public:
  cache_aligned_array(){for(std::size_t n=0;n<N;)::new (data(n++)) T();}
  ~cache_aligned_array(){for(auto n=N;n>0;)data(n--)->~T();}
  cache_aligned_array(const cache_aligned_array&)=delete;
  cache_aligned_array& operator=(const cache_aligned_array&)=delete;

  T& operator[](std::size_t pos)noexcept{return *data(pos);}

private:
  static constexpr std::size_t element_offset=
    (sizeof(T)+cacheline_size-1)/cacheline_size*cacheline_size;

  BOOST_UNORDERED_STATIC_ASSERT(alignof(T)<=cacheline_size);

  T* data(std::size_t pos)noexcept
  {
    return reinterpret_cast<T*>(
      (reinterpret_cast<uintptr_t>(&buf)+cacheline_size-1)/
        cacheline_size*cacheline_size
      +pos*element_offset);
  }

  unsigned char buf[element_offset*N+cacheline_size-1];
};

template<typename Mutex,std::size_t N>
class multimutex
{
public:
  constexpr std::size_t size()const noexcept{return N;}

  Mutex& operator[](std::size_t pos)noexcept
  {
    BOOST_ASSERT(pos<N);
    return mutexes[pos];
  }

  void lock()noexcept{for(std::size_t n=0;n<N;)mutexes[n++].lock();}
  void unlock()noexcept{for(auto n=N;n>0;)mutexes[--n].unlock();}

private:
  cache_aligned_array<Mutex,N> mutexes;
};

/* std::shared_lock is C++14 */

template<typename Mutex>
class shared_lock
{
public:
  shared_lock(Mutex& m_)noexcept:m(m_){m.lock_shared();}
  ~shared_lock()noexcept{if(owns)m.unlock_shared();}

  /* not used but VS in pre-C++17 mode needs to see it for RVO */
  shared_lock(const shared_lock&);

  void lock(){BOOST_ASSERT(!owns);m.lock_shared();owns=true;}
  void unlock(){BOOST_ASSERT(owns);m.unlock_shared();owns=false;}

private:
  Mutex &m;
  bool owns=true;
};

/* VS in pre-C++17 mode can't implement RVO for std::lock_guard due to
 * its copy constructor being deleted.
 */

template<typename Mutex>
class lock_guard
{
public:
  lock_guard(Mutex& m_)noexcept:m(m_){m.lock();}
  ~lock_guard()noexcept{m.unlock();}

  /* not used but VS in pre-C++17 mode needs to see it for RVO */
  lock_guard(const lock_guard&);

private:
  Mutex &m;
};

/* inspired by boost/multi_index/detail/scoped_bilock.hpp */

template<typename Mutex>
class scoped_bilock
{
public:
  scoped_bilock(Mutex& m1,Mutex& m2)noexcept
  {
    bool mutex_lt=std::less<Mutex*>{}(&m1,&m2);

    pm1=mutex_lt?&m1:&m2;
    pm1->lock();
    if(&m1==&m2){
      pm2=nullptr;
    }
    else{
      pm2=mutex_lt?&m2:&m1;
      pm2->lock();
    }
  }

  /* not used but VS in pre-C++17 mode needs to see it for RVO */
  scoped_bilock(const scoped_bilock&);

  ~scoped_bilock()noexcept
  {
    if(pm2)pm2->unlock();
    pm1->unlock();
  }

private:
  Mutex *pm1,*pm2;
};

/* use atomics for group metadata storage */

template<typename Integral>
struct atomic_integral
{
  operator Integral()const{return n.load(std::memory_order_relaxed);}
  void operator=(Integral m){n.store(m,std::memory_order_relaxed);}
  void operator|=(Integral m){n.fetch_or(m,std::memory_order_relaxed);}
  void operator&=(Integral m){n.fetch_and(m,std::memory_order_relaxed);}

  atomic_integral& operator=(atomic_integral const& rhs) {
    n.store(rhs.n.load(std::memory_order_relaxed),std::memory_order_relaxed);
    return *this;
  }

  std::atomic<Integral> n;
};

/* Group-level concurrency protection. It provides a rw mutex plus an
 * atomic insertion counter for optimistic insertion (see
 * unprotected_norehash_emplace_and_visit).
 */

struct group_access
{    
  using mutex_type=rw_spinlock;
  using shared_lock_guard=shared_lock<mutex_type>;
  using exclusive_lock_guard=lock_guard<mutex_type>;
  using insert_counter_type=std::atomic<boost::uint32_t>;

  shared_lock_guard    shared_access(){return shared_lock_guard{m};}
  exclusive_lock_guard exclusive_access(){return exclusive_lock_guard{m};}
  insert_counter_type& insert_counter(){return cnt;}

private:
  mutex_type          m;
  insert_counter_type cnt{0};
};

template<std::size_t Size>
group_access* dummy_group_accesses()
{
  /* Default group_access array to provide to empty containers without
   * incurring dynamic allocation. Mutexes won't actually ever be used,
   * (no successful reduced hash match) and insertion counters won't ever
   * be incremented (insertions won't succeed as capacity()==0).
   */

  static group_access accesses[Size];

  return accesses;
}

/* subclasses table_arrays to add an additional group_access array */

template<typename Value,typename Group,typename SizePolicy,typename Allocator>
struct concurrent_table_arrays:table_arrays<Value,Group,SizePolicy,Allocator>
{
  using group_access_allocator_type=
    typename boost::allocator_rebind<Allocator,group_access>::type;
  using group_access_pointer=
    typename boost::allocator_pointer<group_access_allocator_type>::type;

  using super=table_arrays<Value,Group,SizePolicy,Allocator>;
  using allocator_type=typename super::allocator_type;

  concurrent_table_arrays(const super& arrays,group_access_pointer pga):
    super{arrays},group_accesses_{pga}{}

  group_access* group_accesses()const noexcept{
    return boost::to_address(group_accesses_);
  }

  static concurrent_table_arrays new_(allocator_type al,std::size_t n)
  {
    super x{super::new_(al,n)};
    BOOST_TRY{
      return new_group_access(group_access_allocator_type(al),x);
    }
    BOOST_CATCH(...){
      super::delete_(al,x);
      BOOST_RETHROW
    }
    BOOST_CATCH_END
  }

  static void set_group_access(
    group_access_allocator_type al,concurrent_table_arrays& arrays)
  {
    set_group_access(
      al,arrays,std::is_same<group_access*,group_access_pointer>{});
  }

  static void set_group_access(
    group_access_allocator_type al,
    concurrent_table_arrays& arrays,
    std::false_type /* fancy pointers */)
  {
    arrays.group_accesses_=
        boost::allocator_allocate(al,arrays.groups_size_mask+1);

      for(std::size_t i=0;i<arrays.groups_size_mask+1;++i){
        ::new (arrays.group_accesses()+i) group_access();
      }
  }

  static void set_group_access(
    group_access_allocator_type al,
    concurrent_table_arrays& arrays,
    std::true_type /* optimize when elements() is null */)
  {
    if(!arrays.elements()){
      arrays.group_accesses_=
        dummy_group_accesses<SizePolicy::min_size()>();
    } else {
      set_group_access(al,arrays,std::false_type{});
    }
  }

  static concurrent_table_arrays new_group_access(
    group_access_allocator_type al,const super& x)
  {
    concurrent_table_arrays arrays{x,nullptr};
    set_group_access(al,arrays);
    return arrays;
  }

  static void delete_(allocator_type al,concurrent_table_arrays& arrays)noexcept
  {
    delete_group_access(group_access_allocator_type(al),arrays);
    super::delete_(al,arrays);
  }

  static void delete_group_access(
    group_access_allocator_type al,concurrent_table_arrays& arrays)noexcept
  {
    if(arrays.elements()){
      boost::allocator_deallocate(
        al,arrays.group_accesses_,arrays.groups_size_mask+1);
    }
  }

  group_access_pointer group_accesses_;
};

struct atomic_size_control
{
  static constexpr auto atomic_size_t_size=sizeof(std::atomic<std::size_t>);
  BOOST_UNORDERED_STATIC_ASSERT(atomic_size_t_size<cacheline_size);

  atomic_size_control(std::size_t ml_,std::size_t size_):
    pad0_{},ml{ml_},pad1_{},size{size_}{}
  atomic_size_control(const atomic_size_control& x):
    pad0_{},ml{x.ml.load()},pad1_{},size{x.size.load()}{}

  /* padding to avoid false sharing internally and with sorrounding data */

  unsigned char            pad0_[cacheline_size-atomic_size_t_size];
  std::atomic<std::size_t> ml;
  unsigned char            pad1_[cacheline_size-atomic_size_t_size];
  std::atomic<std::size_t> size;
};

/* std::swap can't be used on non-assignable atomics */

inline void
swap_atomic_size_t(std::atomic<std::size_t>& x,std::atomic<std::size_t>& y)
{
  std::size_t tmp=x;
  x=static_cast<std::size_t>(y);
  y=tmp;
}

inline void swap(atomic_size_control& x,atomic_size_control& y)
{
  swap_atomic_size_t(x.ml,y.ml);
  swap_atomic_size_t(x.size,y.size);
}

/* foa::concurrent_table serves as the foundation for end-user concurrent
 * hash containers.
 * 
 * The exposed interface (completed by the wrapping containers) is not that
 * of a regular container (in fact, it does not model Container as understood
 * by the C++ standard):
 * 
 *   - Iterators are not provided as they are not suitable for concurrent
 *     scenarios.
 *   - As a consequence, composite operations with regular containers
 *     (like, for instance, looking up an element and modifying it), must
 *     be provided natively without any intervening iterator/accesor.
 *     Visitation is a core concept in this design, either on its own (eg.
 *     visit(k) locates the element with key k *and* accesses it) or as part
 *     of a native composite operation (eg. try_emplace_or_visit). Visitation
 *     is constant or mutating depending on whether the used table function is
 *     const or not.
 *   - The API provides member functions for all the meaningful composite
 *     operations of the form "X (and|or) Y", where X, Y are one of the
 *     primitives FIND, ACCESS, INSERT or ERASE.
 *   - Parallel versions of [c]visit_all(f) and erase_if(f) are provided based
 *     on C++17 stdlib parallel algorithms.
 * 
 * Consult boost::concurrent_(flat|node)_(map|set) docs for the full API
 * reference. Heterogeneous lookup is suported by default, that is, without
 * checking for any ::is_transparent typedefs --this checking is done by the
 * wrapping containers.
 *
 * Thread-safe concurrency is implemented using a two-level lock system:
 * 
 *   - A first container-level lock is implemented with an array of
 *     rw spinlocks acting as a single rw mutex with very little
 *     cache-coherence traffic on read (each thread is assigned a different
 *     spinlock in the array). Container-level write locking is only used for
 *     rehashing and other container-wide operations (assignment, swap, etc.)
 *   - Each group of slots has an associated rw spinlock. A thread holds
 *     at most one group lock at any given time. Lookup is implemented in
 *     a (groupwise) lock-free manner until a reduced hash match is found, in
 *     which case the relevant group is locked and the slot is double-checked
 *     for occupancy and compared with the key.
 *   - Each group has also an associated so-called insertion counter used for
 *     the following optimistic insertion algorithm:
 *     - The value of the insertion counter for the initial group in the probe
 *       sequence is locally recorded (let's call this value c0).
 *     - Lookup is as described above. If lookup finds no equivalent element,
 *       search for an available slot for insertion successively locks/unlocks
 *       each group in the probing sequence.
 *     - When an available slot is located, it is preemptively occupied (its
 *       reduced hash value is set) and the insertion counter is atomically
 *       incremented: if no other thread has incremented the counter during the
 *       whole operation (which is checked by comparing with c0), then we're
 *       good to go and complete the insertion, otherwise we roll back and
 *       start over.
 */

template<typename,typename,typename,typename>
class table; /* concurrent/non-concurrent interop */

template <typename TypePolicy,typename Hash,typename Pred,typename Allocator>
using concurrent_table_core_impl=table_core<
  TypePolicy,group15<atomic_integral>,concurrent_table_arrays,
  atomic_size_control,Hash,Pred,Allocator>;

#include <boost/unordered/detail/foa/ignore_wshadow.hpp>

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4714) /* marked as __forceinline not inlined */
#endif

template<typename TypePolicy,typename Hash,typename Pred,typename Allocator>
class concurrent_table:
  concurrent_table_core_impl<TypePolicy,Hash,Pred,Allocator>
{
  using super=concurrent_table_core_impl<TypePolicy,Hash,Pred,Allocator>;
  using type_policy=typename super::type_policy;
  using group_type=typename super::group_type;
  using super::N;
  using prober=typename super::prober;
  using arrays_type=typename super::arrays_type;
  using size_ctrl_type=typename super::size_ctrl_type;
  using compatible_nonconcurrent_table=table<TypePolicy,Hash,Pred,Allocator>;
  friend compatible_nonconcurrent_table;

public:
  using key_type=typename super::key_type;
  using init_type=typename super::init_type;
  using value_type=typename super::value_type;
  using element_type=typename super::element_type;
  using hasher=typename super::hasher;
  using key_equal=typename super::key_equal;
  using allocator_type=typename super::allocator_type;
  using size_type=typename super::size_type;
  static constexpr std::size_t bulk_visit_size=16;

#if defined(BOOST_UNORDERED_ENABLE_STATS)
  using stats=typename super::stats;
#endif

private:
  template<typename Value,typename T>
  using enable_if_is_value_type=typename std::enable_if<
    !std::is_same<init_type,value_type>::value&&
    std::is_same<Value,value_type>::value,
    T
  >::type;

public:
  concurrent_table(
    std::size_t n=default_bucket_count,const Hash& h_=Hash(),
    const Pred& pred_=Pred(),const Allocator& al_=Allocator()):
    super{n,h_,pred_,al_}
    {}

  concurrent_table(const concurrent_table& x):
    concurrent_table(x,x.exclusive_access()){}
  concurrent_table(concurrent_table&& x):
    concurrent_table(std::move(x),x.exclusive_access()){}
  concurrent_table(const concurrent_table& x,const Allocator& al_):
    concurrent_table(x,al_,x.exclusive_access()){}
  concurrent_table(concurrent_table&& x,const Allocator& al_):
    concurrent_table(std::move(x),al_,x.exclusive_access()){}

  template<typename ArraysType>
  concurrent_table(
    compatible_nonconcurrent_table&& x,
    arrays_holder<ArraysType,Allocator>&& ah):
    super{
      std::move(x.h()),std::move(x.pred()),std::move(x.al()),
      [&x]{return arrays_type::new_group_access(
        x.al(),typename arrays_type::super{
          x.arrays.groups_size_index,x.arrays.groups_size_mask,
          to_pointer<typename arrays_type::group_type_pointer>(
            reinterpret_cast<group_type*>(x.arrays.groups())),
          x.arrays.elements_});},
      size_ctrl_type{x.size_ctrl.ml,x.size_ctrl.size}}
  {
    x.arrays=ah.release();
    x.size_ctrl.ml=x.initial_max_load();
    x.size_ctrl.size=0;
    BOOST_UNORDERED_SWAP_STATS(this->cstats,x.cstats);
  }

  concurrent_table(compatible_nonconcurrent_table&& x):
    concurrent_table(std::move(x),x.make_empty_arrays())
  {}

  ~concurrent_table()=default;

  concurrent_table& operator=(const concurrent_table& x)
  {
    auto lck=exclusive_access(*this,x);
    super::operator=(x);
    return *this;
  }

  concurrent_table& operator=(concurrent_table&& x)noexcept(
    noexcept(std::declval<super&>() = std::declval<super&&>()))
  {
    auto lck=exclusive_access(*this,x);
    super::operator=(std::move(x));
    return *this;
  }

  concurrent_table& operator=(std::initializer_list<value_type> il) {
    auto lck=exclusive_access();
    super::clear();
    super::noshrink_reserve(il.size());
    for (auto const& v : il) {
      this->unprotected_emplace(v);
    }
    return *this;
  }

  allocator_type get_allocator()const noexcept
  {
    auto lck=shared_access();
    return super::get_allocator();
  }

  template<typename Key,typename F>
  BOOST_FORCEINLINE std::size_t visit(const Key& x,F&& f)
  {
    return visit_impl(group_exclusive{},x,std::forward<F>(f));
  }

  template<typename Key,typename F>
  BOOST_FORCEINLINE std::size_t visit(const Key& x,F&& f)const
  {
    return visit_impl(group_shared{},x,std::forward<F>(f));
  }

  template<typename Key,typename F>
  BOOST_FORCEINLINE std::size_t cvisit(const Key& x,F&& f)const
  {
    return visit(x,std::forward<F>(f));
  }

  template<typename FwdIterator,typename F>
  BOOST_FORCEINLINE
  std::size_t visit(FwdIterator first,FwdIterator last,F&& f)
  {
    return bulk_visit_impl(group_exclusive{},first,last,std::forward<F>(f));
  }

  template<typename FwdIterator,typename F>
  BOOST_FORCEINLINE
  std::size_t visit(FwdIterator first,FwdIterator last,F&& f)const
  {
    return bulk_visit_impl(group_shared{},first,last,std::forward<F>(f));
  }

  template<typename FwdIterator,typename F>
  BOOST_FORCEINLINE
  std::size_t cvisit(FwdIterator first,FwdIterator last,F&& f)const
  {
    return visit(first,last,std::forward<F>(f));
  }

  template<typename F> std::size_t visit_all(F&& f)
  {
    return visit_all_impl(group_exclusive{},std::forward<F>(f));
  }

  template<typename F> std::size_t visit_all(F&& f)const
  {
    return visit_all_impl(group_shared{},std::forward<F>(f));
  }

  template<typename F> std::size_t cvisit_all(F&& f)const
  {
    return visit_all(std::forward<F>(f));
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename ExecutionPolicy,typename F>
  void visit_all(ExecutionPolicy&& policy,F&& f)
  {
    visit_all_impl(
      group_exclusive{},
      std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }

  template<typename ExecutionPolicy,typename F>
  void visit_all(ExecutionPolicy&& policy,F&& f)const
  {
    visit_all_impl(
      group_shared{},
      std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }

  template<typename ExecutionPolicy,typename F>
  void cvisit_all(ExecutionPolicy&& policy,F&& f)const
  {
    visit_all(std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }
#endif

  template<typename F> bool visit_while(F&& f)
  {
    return visit_while_impl(group_exclusive{},std::forward<F>(f));
  }

  template<typename F> bool visit_while(F&& f)const
  {
    return visit_while_impl(group_shared{},std::forward<F>(f));
  }

  template<typename F> bool cvisit_while(F&& f)const
  {
    return visit_while(std::forward<F>(f));
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename ExecutionPolicy,typename F>
  bool visit_while(ExecutionPolicy&& policy,F&& f)
  {
    return visit_while_impl(
      group_exclusive{},
      std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }

  template<typename ExecutionPolicy,typename F>
  bool visit_while(ExecutionPolicy&& policy,F&& f)const
  {
    return visit_while_impl(
      group_shared{},
      std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }

  template<typename ExecutionPolicy,typename F>
  bool cvisit_while(ExecutionPolicy&& policy,F&& f)const
  {
    return visit_while(
      std::forward<ExecutionPolicy>(policy),std::forward<F>(f));
  }
#endif

  bool empty()const noexcept{return size()==0;}
  
  std::size_t size()const noexcept
  {
    auto lck=shared_access();
    return unprotected_size();
  }

  using super::max_size; 

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace(Args&&... args)
  {
    return construct_and_emplace(std::forward<Args>(args)...);
  }

  /* Optimization for value_type and init_type, to avoid constructing twice */
  template<typename Value>
  BOOST_FORCEINLINE auto emplace(Value&& x)->typename std::enable_if<
    detail::is_similar_to_any<Value,value_type,init_type>::value,bool>::type
  {
    return emplace_impl(std::forward<Value>(x));
  }

  /* Optimizations for maps for (k,v) to avoid eagerly constructing value */
  template <typename K, typename V>
  BOOST_FORCEINLINE auto emplace(K&& k, V&& v) ->
    typename std::enable_if<is_emplace_kv_able<concurrent_table, K>::value,
      bool>::type
  {
    alloc_cted_or_fwded_key_type<type_policy, Allocator, K&&> x(
      this->al(), std::forward<K>(k));
    return emplace_impl(
      try_emplace_args_t{}, x.move_or_fwd(), std::forward<V>(v));
  }

  BOOST_FORCEINLINE bool
  insert(const init_type& x){return emplace_impl(x);}

  BOOST_FORCEINLINE bool
  insert(init_type&& x){return emplace_impl(std::move(x));}

  /* template<typename=void> tilts call ambiguities in favor of init_type */

  template<typename=void>
  BOOST_FORCEINLINE bool
  insert(const value_type& x){return emplace_impl(x);}
  
  template<typename=void>
  BOOST_FORCEINLINE bool
  insert(value_type&& x){return emplace_impl(std::move(x));}

  template<typename T=element_type>
  BOOST_FORCEINLINE
  typename std::enable_if<
    !std::is_same<T,value_type>::value,
    bool
  >::type
  insert(element_type&& x){return emplace_impl(std::move(x));}

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE bool try_emplace(Key&& x,Args&&... args)
  {
    return emplace_impl(
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE bool try_emplace_or_visit(Key&& x,Args&&... args)
  {
    return emplace_or_visit_flast(
      group_exclusive{},
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE bool try_emplace_or_cvisit(Key&& x,Args&&... args)
  {
    return emplace_or_visit_flast(
      group_shared{},
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE bool try_emplace_and_visit(Key&& x,Args&&... args)
  {
    return emplace_and_visit_flast(
      group_exclusive{},
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  template<typename Key,typename... Args>
  BOOST_FORCEINLINE bool try_emplace_and_cvisit(Key&& x,Args&&... args)
  {
    return emplace_and_visit_flast(
      group_shared{},
      try_emplace_args_t{},std::forward<Key>(x),std::forward<Args>(args)...);
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace_or_visit(Args&&... args)
  {
    return construct_and_emplace_or_visit_flast(
      group_exclusive{},std::forward<Args>(args)...);
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace_or_cvisit(Args&&... args)
  {
    return construct_and_emplace_or_visit_flast(
      group_shared{},std::forward<Args>(args)...);
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace_and_visit(Args&&... args)
  {
    return construct_and_emplace_and_visit_flast(
      group_exclusive{},std::forward<Args>(args)...);
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace_and_cvisit(Args&&... args)
  {
    return construct_and_emplace_and_visit_flast(
      group_shared{},std::forward<Args>(args)...);
  }

  template<typename Value,typename F>
  BOOST_FORCEINLINE bool insert_or_visit(Value&& x,F&& f)
  {
    return insert_and_visit(
      std::forward<Value>(x),[](const value_type&){},std::forward<F>(f));
  }

  template<typename Value,typename F>
  BOOST_FORCEINLINE bool insert_or_cvisit(Value&& x,F&& f)
  {
    return insert_and_cvisit(
      std::forward<Value>(x),[](const value_type&){},std::forward<F>(f));
  }

  template<typename F1,typename F2>
  BOOST_FORCEINLINE bool insert_and_visit(const init_type& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_exclusive{},std::forward<F1>(f1),std::forward<F2>(f2),x);
  }

  template<typename F1,typename F2>
  BOOST_FORCEINLINE bool insert_and_cvisit(const init_type& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_shared{},std::forward<F1>(f1),std::forward<F2>(f2),x);
  }

  template<typename F1,typename F2>
  BOOST_FORCEINLINE bool insert_and_visit(init_type&& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_exclusive{},std::forward<F1>(f1),std::forward<F2>(f2),
      std::move(x));
  }

  template<typename F1,typename F2>
  BOOST_FORCEINLINE bool insert_and_cvisit(init_type&& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_shared{},std::forward<F1>(f1),std::forward<F2>(f2),std::move(x));
  }

  /* SFINAE tilts call ambiguities in favor of init_type */

  template<typename Value,typename F1,typename F2>
  BOOST_FORCEINLINE auto insert_and_visit(const Value& x,F1&& f1,F2&& f2)
    ->enable_if_is_value_type<Value,bool>
  {
    return emplace_and_visit_impl(
      group_exclusive{},std::forward<F1>(f1),std::forward<F2>(f2),x);
  }

  template<typename Value,typename F1,typename F2>
  BOOST_FORCEINLINE auto insert_and_cvisit(const Value& x,F1&& f1,F2&& f2)
    ->enable_if_is_value_type<Value,bool>
  {
    return emplace_and_visit_impl(
      group_shared{},std::forward<F1>(f1),std::forward<F2>(f2),x);
  }

  template<typename Value,typename F1,typename F2>
  BOOST_FORCEINLINE auto insert_and_visit(Value&& x,F1&& f1,F2&& f2)
    ->enable_if_is_value_type<Value,bool>
  {
    return emplace_and_visit_impl(
      group_exclusive{},std::forward<F1>(f1),std::forward<F2>(f2),
      std::move(x));
  }

  template<typename Value,typename F1,typename F2>
  BOOST_FORCEINLINE auto insert_and_cvisit(Value&& x,F1&& f1,F2&& f2)
    ->enable_if_is_value_type<Value,bool>
  {
    return emplace_and_visit_impl(
      group_shared{},std::forward<F1>(f1),std::forward<F2>(f2),std::move(x));
  }

  template<typename F1,typename F2,typename T=element_type>
  BOOST_FORCEINLINE
  typename std::enable_if<
    !std::is_same<T,value_type>::value,
    bool
  >::type
  insert_and_visit(element_type&& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_exclusive{},std::forward<F1>(f1),std::forward<F2>(f2),
      std::move(x));
  }

  template<typename F1,typename F2,typename T=element_type>
  BOOST_FORCEINLINE
  typename std::enable_if<
    !std::is_same<T,value_type>::value,
    bool
  >::type
  insert_and_cvisit(element_type&& x,F1&& f1,F2&& f2)
  {
    return emplace_and_visit_impl(
      group_shared{},std::forward<F1>(f1),std::forward<F2>(f2),std::move(x));
  }

  template<typename Key>
  BOOST_FORCEINLINE std::size_t erase(const Key& x)
  {
    return erase_if(x,[](const value_type&){return true;});
  }

  template<typename Key,typename F>
  BOOST_FORCEINLINE auto erase_if(const Key& x,F&& f)->typename std::enable_if<
    !is_execution_policy<Key>::value,std::size_t>::type
  {
    auto        lck=shared_access();
    auto        hash=this->hash_for(x);
    std::size_t res=0;
    unprotected_internal_visit(
      group_exclusive{},x,this->position_for(hash),hash,
      [&,this](group_type* pg,unsigned int n,element_type* p)
      {
        if(f(cast_for(group_exclusive{},type_policy::value_from(*p)))){
          super::erase(pg,n,p);
          res=1;
        }
      });
    return res;
  }

  template<typename F>
  std::size_t erase_if(F&& f)
  {
    auto        lck=shared_access();
    std::size_t res=0;
    for_all_elements(
      group_exclusive{},
      [&,this](group_type* pg,unsigned int n,element_type* p){
        if(f(cast_for(group_exclusive{},type_policy::value_from(*p)))){
          super::erase(pg,n,p);
          ++res;
        }
      });
    return res;
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename ExecutionPolicy,typename F>
  auto erase_if(ExecutionPolicy&& policy,F&& f)->typename std::enable_if<
    is_execution_policy<ExecutionPolicy>::value,void>::type
  {
    auto lck=shared_access();
    for_all_elements(
      group_exclusive{},std::forward<ExecutionPolicy>(policy),
      [&,this](group_type* pg,unsigned int n,element_type* p){
        if(f(cast_for(group_exclusive{},type_policy::value_from(*p)))){
          super::erase(pg,n,p);
        }
      });
  }
#endif

  void swap(concurrent_table& x)
    noexcept(noexcept(std::declval<super&>().swap(std::declval<super&>())))
  {
    auto lck=exclusive_access(*this,x);
    super::swap(x);
  }

  void clear()noexcept
  {
    auto lck=exclusive_access();
    super::clear();
  }

  template<typename Key,typename Extractor>
  BOOST_FORCEINLINE void extract(const Key& x,Extractor&& ext)
  {
    extract_if(
      x,[](const value_type&){return true;},std::forward<Extractor>(ext));
  }

  template<typename Key,typename F,typename Extractor>
  BOOST_FORCEINLINE void extract_if(const Key& x,F&& f,Extractor&& ext)
  {
    auto        lck=shared_access();
    auto        hash=this->hash_for(x);
    unprotected_internal_visit(
      group_exclusive{},x,this->position_for(hash),hash,
      [&,this](group_type* pg,unsigned int n,element_type* p)
      {
        if(f(cast_for(group_exclusive{},type_policy::value_from(*p)))){
          ext(std::move(*p),this->al());
          super::erase(pg,n,p);
        }
      });
  }

  // TODO: should we accept different allocator too?
  template<typename Hash2,typename Pred2>
  size_type merge(concurrent_table<TypePolicy,Hash2,Pred2,Allocator>& x)
  {
    using merge_table_type=concurrent_table<TypePolicy,Hash2,Pred2,Allocator>;
    using super2=typename merge_table_type::super;

    // for clang
    boost::ignore_unused<super2>();

    auto      lck=exclusive_access(*this,x);
    size_type s=super::size();
    x.super2::for_all_elements( /* super2::for_all_elements -> unprotected */
      [&,this](group_type* pg,unsigned int n,element_type* p){
        typename merge_table_type::erase_on_exit e{x,pg,n,p};
        if(!unprotected_emplace(type_policy::move(*p)))e.rollback();
      });
    return size_type{super::size()-s};
  }

  template<typename Hash2,typename Pred2>
  void merge(concurrent_table<TypePolicy,Hash2,Pred2,Allocator>&& x){merge(x);}

  hasher hash_function()const
  {
    auto lck=shared_access();
    return super::hash_function();
  }

  key_equal key_eq()const
  {
    auto lck=shared_access();
    return super::key_eq();
  }

  template<typename Key>
  BOOST_FORCEINLINE std::size_t count(Key&& x)const
  {
    return (std::size_t)contains(std::forward<Key>(x));
  }

  template<typename Key>
  BOOST_FORCEINLINE bool contains(Key&& x)const
  {
    return visit(std::forward<Key>(x),[](const value_type&){})!=0;
  }

  std::size_t capacity()const noexcept
  {
    auto lck=shared_access();
    return super::capacity();
  }

  float load_factor()const noexcept
  {
    auto lck=shared_access();
    if(super::capacity()==0)return 0;
    else                    return float(unprotected_size())/
                                   float(super::capacity());
  }

  using super::max_load_factor;

  std::size_t max_load()const noexcept
  {
    auto lck=shared_access();
    return super::max_load();
  }

  void rehash(std::size_t n)
  {
    auto lck=exclusive_access();
    super::rehash(n);
  }

  void reserve(std::size_t n)
  {
    auto lck=exclusive_access();
    super::reserve(n);
  }

#if defined(BOOST_UNORDERED_ENABLE_STATS)
  /* already thread safe */

  using super::get_stats;
  using super::reset_stats;
#endif

  template<typename Predicate>
  friend std::size_t erase_if(concurrent_table& x,Predicate&& pr)
  {
    return x.erase_if(std::forward<Predicate>(pr));
  }

  friend bool operator==(const concurrent_table& x,const concurrent_table& y)
  {
    auto lck=exclusive_access(x,y);
    return static_cast<const super&>(x)==static_cast<const super&>(y);
  }

  friend bool operator!=(const concurrent_table& x,const concurrent_table& y)
  {
    return !(x==y);
  }

private:
  template<typename,typename,typename,typename> friend class concurrent_table;

  using mutex_type=rw_spinlock;
  using multimutex_type=multimutex<mutex_type,128>; // TODO: adapt 128 to the machine
  using shared_lock_guard=reentrancy_checked<shared_lock<mutex_type>>;
  using exclusive_lock_guard=reentrancy_checked<lock_guard<multimutex_type>>;
  using exclusive_bilock_guard=
    reentrancy_bichecked<scoped_bilock<multimutex_type>>;
  using group_shared_lock_guard=typename group_access::shared_lock_guard;
  using group_exclusive_lock_guard=typename group_access::exclusive_lock_guard;
  using group_insert_counter_type=typename group_access::insert_counter_type;

  concurrent_table(const concurrent_table& x,exclusive_lock_guard):
    super{x}{}
  concurrent_table(concurrent_table&& x,exclusive_lock_guard):
    super{std::move(x)}{}
  concurrent_table(
    const concurrent_table& x,const Allocator& al_,exclusive_lock_guard):
    super{x,al_}{}
  concurrent_table(
    concurrent_table&& x,const Allocator& al_,exclusive_lock_guard):
    super{std::move(x),al_}{}

  inline shared_lock_guard shared_access()const
  {
    thread_local auto id=(++thread_counter)%mutexes.size();

    return shared_lock_guard{this,mutexes[id]};
  }

  inline exclusive_lock_guard exclusive_access()const
  {
    return exclusive_lock_guard{this,mutexes};
  }

  static inline exclusive_bilock_guard exclusive_access(
    const concurrent_table& x,const concurrent_table& y)
  {
    return {&x,&y,x.mutexes,y.mutexes};
  }

  template<typename Hash2,typename Pred2>
  static inline exclusive_bilock_guard exclusive_access(
    const concurrent_table& x,
    const concurrent_table<TypePolicy,Hash2,Pred2,Allocator>& y)
  {
    return {&x,&y,x.mutexes,y.mutexes};
  }

  /* Tag-dispatched shared/exclusive group access */

  using group_shared=std::false_type;
  using group_exclusive=std::true_type;

  inline group_shared_lock_guard access(group_shared,std::size_t pos)const
  {
    return this->arrays.group_accesses()[pos].shared_access();
  }

  inline group_exclusive_lock_guard access(
    group_exclusive,std::size_t pos)const
  {
    return this->arrays.group_accesses()[pos].exclusive_access();
  }

  inline group_insert_counter_type& insert_counter(std::size_t pos)const
  {
    return this->arrays.group_accesses()[pos].insert_counter();
  }

  /* Const casts value_type& according to the level of group access for
   * safe passing to visitation functions. When type_policy is set-like,
   * access is always const regardless of group access.
   */

  static inline const value_type&
  cast_for(group_shared,value_type& x){return x;}

  static inline typename std::conditional<
    std::is_same<key_type,value_type>::value,
    const value_type&,
    value_type&
  >::type
  cast_for(group_exclusive,value_type& x){return x;}

  struct erase_on_exit
  {
    erase_on_exit(
      concurrent_table& x_,
      group_type* pg_,unsigned int pos_,element_type* p_):
      x(x_),pg(pg_),pos(pos_),p(p_){}
    ~erase_on_exit(){if(!rollback_)x.super::erase(pg,pos,p);}

    void rollback(){rollback_=true;}

    concurrent_table &x;
    group_type       *pg;
    unsigned  int     pos;
    element_type     *p;
    bool              rollback_=false;
  };

  template<typename GroupAccessMode,typename Key,typename F>
  BOOST_FORCEINLINE std::size_t visit_impl(
    GroupAccessMode access_mode,const Key& x,F&& f)const
  {
    auto lck=shared_access();
    auto hash=this->hash_for(x);
    return unprotected_visit(
      access_mode,x,this->position_for(hash),hash,std::forward<F>(f));
  }

  template<typename GroupAccessMode,typename FwdIterator,typename F>
  BOOST_FORCEINLINE
  std::size_t bulk_visit_impl(
    GroupAccessMode access_mode,FwdIterator first,FwdIterator last,F&& f)const
  {
    auto        lck=shared_access();
    std::size_t res=0;
    auto        n=static_cast<std::size_t>(std::distance(first,last));
    while(n){
      auto m=n<2*bulk_visit_size?n:bulk_visit_size;
      res+=unprotected_bulk_visit(access_mode,first,m,std::forward<F>(f));
      n-=m;
      std::advance(
        first,
        static_cast<
          typename std::iterator_traits<FwdIterator>::difference_type>(m));
    }
    return res;
  }

  template<typename GroupAccessMode,typename F>
  std::size_t visit_all_impl(GroupAccessMode access_mode,F&& f)const
  {
    auto lck=shared_access();
    std::size_t res=0;
    for_all_elements(access_mode,[&](element_type* p){
      f(cast_for(access_mode,type_policy::value_from(*p)));
      ++res;
    });
    return res;
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename GroupAccessMode,typename ExecutionPolicy,typename F>
  void visit_all_impl(
    GroupAccessMode access_mode,ExecutionPolicy&& policy,F&& f)const
  {
    auto lck=shared_access();
    for_all_elements(
      access_mode,std::forward<ExecutionPolicy>(policy),
      [&](element_type* p){
        f(cast_for(access_mode,type_policy::value_from(*p)));
      });
  }
#endif

  template<typename GroupAccessMode,typename F>
  bool visit_while_impl(GroupAccessMode access_mode,F&& f)const
  {
    auto lck=shared_access();
    return for_all_elements_while(access_mode,[&](element_type* p){
      return f(cast_for(access_mode,type_policy::value_from(*p)));
    });
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename GroupAccessMode,typename ExecutionPolicy,typename F>
  bool visit_while_impl(
    GroupAccessMode access_mode,ExecutionPolicy&& policy,F&& f)const
  {
    auto lck=shared_access();
    return for_all_elements_while(
      access_mode,std::forward<ExecutionPolicy>(policy),
      [&](element_type* p){
        return f(cast_for(access_mode,type_policy::value_from(*p)));
      });
  }
#endif

  template<typename GroupAccessMode,typename Key,typename F>
  BOOST_FORCEINLINE std::size_t unprotected_visit(
    GroupAccessMode access_mode,
    const Key& x,std::size_t pos0,std::size_t hash,F&& f)const
  {
    return unprotected_internal_visit(
      access_mode,x,pos0,hash,
      [&](group_type*,unsigned int,element_type* p)
        {f(cast_for(access_mode,type_policy::value_from(*p)));});
  }

#if defined(BOOST_MSVC)
/* warning: forcing value to bool 'true' or 'false' in bool(pred()...) */
#pragma warning(push)
#pragma warning(disable:4800)
#endif

  template<typename GroupAccessMode,typename Key,typename F>
  BOOST_FORCEINLINE std::size_t unprotected_internal_visit(
    GroupAccessMode access_mode,
    const Key& x,std::size_t pos0,std::size_t hash,F&& f)const
  {    
    BOOST_UNORDERED_STATS_COUNTER(num_cmps);
    prober pb(pos0);
    do{
      auto pos=pb.get();
      auto pg=this->arrays.groups()+pos;
      auto mask=pg->match(hash);
      if(mask){
        auto p=this->arrays.elements()+pos*N;
        BOOST_UNORDERED_PREFETCH_ELEMENTS(p,N);
        auto lck=access(access_mode,pos);
        do{
          auto n=unchecked_countr_zero(mask);
          if(BOOST_LIKELY(pg->is_occupied(n))){
            BOOST_UNORDERED_INCREMENT_STATS_COUNTER(num_cmps);
            if(BOOST_LIKELY(bool(this->pred()(x,this->key_from(p[n]))))){
              f(pg,n,p+n);
              BOOST_UNORDERED_ADD_STATS(
                this->cstats.successful_lookup,(pb.length(),num_cmps));
              return 1;
            }
          }
          mask&=mask-1;
        }while(mask);
      }
      if(BOOST_LIKELY(pg->is_not_overflowed(hash))){
        BOOST_UNORDERED_ADD_STATS(
          this->cstats.unsuccessful_lookup,(pb.length(),num_cmps));
        return 0;
      }
    }
    while(BOOST_LIKELY(pb.next(this->arrays.groups_size_mask)));
    BOOST_UNORDERED_ADD_STATS(
      this->cstats.unsuccessful_lookup,(pb.length(),num_cmps));
    return 0;
  }

 template<typename GroupAccessMode,typename FwdIterator,typename F>
  BOOST_FORCEINLINE std::size_t unprotected_bulk_visit(
    GroupAccessMode access_mode,FwdIterator first,std::size_t m,F&& f)const
  {
    BOOST_ASSERT(m<2*bulk_visit_size);

    std::size_t res=0,
                hashes[2*bulk_visit_size-1],
                positions[2*bulk_visit_size-1];
    int         masks[2*bulk_visit_size-1];
    auto        it=first;

    for(auto i=m;i--;++it){
      auto hash=hashes[i]=this->hash_for(*it);
      auto pos=positions[i]=this->position_for(hash);
      BOOST_UNORDERED_PREFETCH(this->arrays.groups()+pos);
    }

    for(auto i=m;i--;){
      auto hash=hashes[i];
      auto pos=positions[i];
      auto mask=masks[i]=(this->arrays.groups()+pos)->match(hash);
      if(mask){
        BOOST_UNORDERED_PREFETCH(this->arrays.group_accesses()+pos);
        BOOST_UNORDERED_PREFETCH(
          this->arrays.elements()+pos*N+unchecked_countr_zero(mask));
      }
    }

    it=first;
    for(auto i=m;i--;++it){
      BOOST_UNORDERED_STATS_COUNTER(num_cmps);
      auto          pos=positions[i];
      prober        pb(pos);
      auto          pg=this->arrays.groups()+pos;
      auto          mask=masks[i];
      element_type *p;
      if(!mask)goto post_mask;
      p=this->arrays.elements()+pos*N;
      for(;;){
        {
          auto lck=access(access_mode,pos);
          do{
            auto n=unchecked_countr_zero(mask);
            if(BOOST_LIKELY(pg->is_occupied(n))){
              BOOST_UNORDERED_INCREMENT_STATS_COUNTER(num_cmps);
              if(bool(this->pred()(*it,this->key_from(p[n])))){
                f(cast_for(access_mode,type_policy::value_from(p[n])));
                ++res;
                BOOST_UNORDERED_ADD_STATS(
                  this->cstats.successful_lookup,(pb.length(),num_cmps));
                goto next_key;
              }
            }
            mask&=mask-1;
          }while(mask);
        }
      post_mask:
        do{
          if(BOOST_LIKELY(pg->is_not_overflowed(hashes[i]))||
             BOOST_UNLIKELY(!pb.next(this->arrays.groups_size_mask))){
            BOOST_UNORDERED_ADD_STATS(
              this->cstats.unsuccessful_lookup,(pb.length(),num_cmps));
            goto next_key;
          }
          pos=pb.get();
          pg=this->arrays.groups()+pos;
          mask=pg->match(hashes[i]);
        }while(!mask);
        p=this->arrays.elements()+pos*N;
        BOOST_UNORDERED_PREFETCH_ELEMENTS(p,N);
      }
      next_key:;
    }
    return res;
  }

#if defined(BOOST_MSVC)
#pragma warning(pop) /* C4800 */
#endif

  std::size_t unprotected_size()const
  {
    std::size_t m=this->size_ctrl.ml;
    std::size_t s=this->size_ctrl.size;
    return s<=m?s:m;
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool construct_and_emplace(Args&&... args)
  {
    return construct_and_emplace_or_visit(
      group_shared{},[](const value_type&){},std::forward<Args>(args)...);
  }

  struct call_construct_and_emplace_or_visit
  {
    template<typename... Args>
    BOOST_FORCEINLINE bool operator()(
      concurrent_table* this_,Args&&... args)const
    {
      return this_->construct_and_emplace_or_visit(
        std::forward<Args>(args)...);
    }
  };

  template<typename GroupAccessMode,typename... Args>
  BOOST_FORCEINLINE bool construct_and_emplace_or_visit_flast(
    GroupAccessMode access_mode,Args&&... args)
  {
    return mp11::tuple_apply(
      call_construct_and_emplace_or_visit{},
      std::tuple_cat(
        std::make_tuple(this,access_mode),
        tuple_rotate_right(std::forward_as_tuple(std::forward<Args>(args)...))
      )
    );
  }

  struct call_construct_and_emplace_and_visit
  {
    template<typename... Args>
    BOOST_FORCEINLINE bool operator()(
      concurrent_table* this_,Args&&... args)const
    {
      return this_->construct_and_emplace_and_visit(
        std::forward<Args>(args)...);
    }
  };

  template<typename GroupAccessMode,typename... Args>
  BOOST_FORCEINLINE bool construct_and_emplace_and_visit_flast(
    GroupAccessMode access_mode,Args&&... args)
  {
    return mp11::tuple_apply(
      call_construct_and_emplace_and_visit{},
      std::tuple_cat(
        std::make_tuple(this,access_mode),
        tuple_rotate_right<2>(
          std::forward_as_tuple(std::forward<Args>(args)...))
      )
    );
  }

  template<typename GroupAccessMode,typename F,typename... Args>
  BOOST_FORCEINLINE bool construct_and_emplace_or_visit(
    GroupAccessMode access_mode,F&& f,Args&&... args)
  {
    return construct_and_emplace_and_visit(
      access_mode,[](const value_type&){},std::forward<F>(f),
      std::forward<Args>(args)...);
  }

  template<typename GroupAccessMode,typename F1,typename F2,typename... Args>
  BOOST_FORCEINLINE bool construct_and_emplace_and_visit(
    GroupAccessMode access_mode,F1&& f1,F2&& f2,Args&&... args)
  {
    auto lck=shared_access();

    alloc_cted_insert_type<type_policy,Allocator,Args...> x(
      this->al(),std::forward<Args>(args)...);
    int res=unprotected_norehash_emplace_and_visit(
      access_mode,std::forward<F1>(f1),std::forward<F2>(f2),
      type_policy::move(x.value()));
    if(BOOST_LIKELY(res>=0))return res!=0;

    lck.unlock();

    rehash_if_full();
    return noinline_emplace_and_visit(
      access_mode,std::forward<F1>(f1),std::forward<F2>(f2),
      type_policy::move(x.value()));
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool emplace_impl(Args&&... args)
  {
    return emplace_or_visit_impl(
      group_shared{},[](const value_type&){},std::forward<Args>(args)...);
  }

  template<typename GroupAccessMode,typename F,typename... Args>
  BOOST_NOINLINE bool noinline_emplace_or_visit(
    GroupAccessMode access_mode,F&& f,Args&&... args)
  {
    return emplace_or_visit_impl(
      access_mode,std::forward<F>(f),std::forward<Args>(args)...);
  }

  template<typename GroupAccessMode,typename F1,typename F2,typename... Args>
  BOOST_NOINLINE bool noinline_emplace_and_visit(
    GroupAccessMode access_mode,F1&& f1,F2&& f2,Args&&... args)
  {
    return emplace_and_visit_impl(
      access_mode,std::forward<F1>(f1),std::forward<F2>(f2),
      std::forward<Args>(args)...);
  }

  struct call_emplace_or_visit_impl
  {
    template<typename... Args>
    BOOST_FORCEINLINE bool operator()(
      concurrent_table* this_,Args&&... args)const
    {
      return this_->emplace_or_visit_impl(std::forward<Args>(args)...);
    }
  };

  template<typename GroupAccessMode,typename... Args>
  BOOST_FORCEINLINE bool emplace_or_visit_flast(
    GroupAccessMode access_mode,Args&&... args)
  {
    return mp11::tuple_apply(
      call_emplace_or_visit_impl{},
      std::tuple_cat(
        std::make_tuple(this,access_mode),
        tuple_rotate_right(std::forward_as_tuple(std::forward<Args>(args)...))
      )
    );
  }

  struct call_emplace_and_visit_impl
  {
    template<typename... Args>
    BOOST_FORCEINLINE bool operator()(
      concurrent_table* this_,Args&&... args)const
    {
      return this_->emplace_and_visit_impl(std::forward<Args>(args)...);
    }
  };

  template<typename GroupAccessMode,typename... Args>
  BOOST_FORCEINLINE bool emplace_and_visit_flast(
    GroupAccessMode access_mode,Args&&... args)
  {
    return mp11::tuple_apply(
      call_emplace_and_visit_impl{},
      std::tuple_cat(
        std::make_tuple(this,access_mode),
        tuple_rotate_right<2>(
          std::forward_as_tuple(std::forward<Args>(args)...))
      )
    );
  }

  template<typename GroupAccessMode,typename F,typename... Args>
  BOOST_FORCEINLINE bool emplace_or_visit_impl(
    GroupAccessMode access_mode,F&& f,Args&&... args)
  {
    return emplace_and_visit_impl(
      access_mode,[](const value_type&){},std::forward<F>(f),
      std::forward<Args>(args)...);
  }

  template<typename GroupAccessMode,typename F1,typename F2,typename... Args>
  BOOST_FORCEINLINE bool emplace_and_visit_impl(
    GroupAccessMode access_mode,F1&& f1,F2&& f2,Args&&... args)
  {
    for(;;){
      {
        auto lck=shared_access();
        int res=unprotected_norehash_emplace_and_visit(
          access_mode,std::forward<F1>(f1),std::forward<F2>(f2),
          std::forward<Args>(args)...);
        if(BOOST_LIKELY(res>=0))return res!=0;
      }
      rehash_if_full();
    }
  }

  template<typename... Args>
  BOOST_FORCEINLINE bool unprotected_emplace(Args&&... args)
  {
    const auto &k=this->key_from(std::forward<Args>(args)...);
    auto        hash=this->hash_for(k);
    auto        pos0=this->position_for(hash);

    if(this->find(k,pos0,hash))return false;

    if(BOOST_LIKELY(this->size_ctrl.size<this->size_ctrl.ml)){
      this->unchecked_emplace_at(pos0,hash,std::forward<Args>(args)...);
    }
    else{
      this->unchecked_emplace_with_rehash(hash,std::forward<Args>(args)...);
    }
    return true;
  }

  template<typename GroupAccessMode,typename F,typename... Args>
  BOOST_FORCEINLINE int
  unprotected_norehash_emplace_or_visit(
    GroupAccessMode access_mode,F&& f,Args&&... args)
  {
    return unprotected_norehash_emplace_and_visit(
      access_mode,[&](const value_type&){},
      std::forward<F>(f),std::forward<Args>(args)...);
  }

  struct reserve_size
  {
    reserve_size(concurrent_table& x_):x(x_)
    {
      size_=++x.size_ctrl.size;
    }

    ~reserve_size()
    {
      if(!commit_)--x.size_ctrl.size;
    }

    bool succeeded()const{return size_<=x.size_ctrl.ml;}

    void commit(){commit_=true;}

    concurrent_table &x;
    std::size_t       size_;
    bool              commit_=false;
  };

  struct reserve_slot
  {
    reserve_slot(group_type* pg_,std::size_t pos_,std::size_t hash):
      pg{pg_},pos{pos_}
    {
      pg->set(pos,hash);
    }

    ~reserve_slot()
    {
      if(!commit_)pg->reset(pos);
    }

    void commit(){commit_=true;}

    group_type  *pg;
    std::size_t pos;
    bool        commit_=false;
  };

  template<typename GroupAccessMode,typename F1,typename F2,typename... Args>
  BOOST_FORCEINLINE int
  unprotected_norehash_emplace_and_visit(
    GroupAccessMode access_mode,F1&& f1,F2&& f2,Args&&... args)
  {
    const auto &k=this->key_from(std::forward<Args>(args)...);
    auto        hash=this->hash_for(k);
    auto        pos0=this->position_for(hash);

    for(;;){
    startover:
      boost::uint32_t counter=insert_counter(pos0);
      if(unprotected_visit(
        access_mode,k,pos0,hash,std::forward<F2>(f2)))return 0;

      reserve_size rsize(*this);
      if(BOOST_LIKELY(rsize.succeeded())){
        for(prober pb(pos0);;pb.next(this->arrays.groups_size_mask)){
          auto pos=pb.get();
          auto pg=this->arrays.groups()+pos;
          auto lck=access(group_exclusive{},pos);
          auto mask=pg->match_available();
          if(BOOST_LIKELY(mask!=0)){
            auto n=unchecked_countr_zero(mask);
            reserve_slot rslot{pg,n,hash};
            if(BOOST_UNLIKELY(insert_counter(pos0)++!=counter)){
              /* other thread inserted from pos0, need to start over */
              goto startover;
            }
            auto p=this->arrays.elements()+pos*N+n;
            this->construct_element(p,std::forward<Args>(args)...);
            rslot.commit();
            rsize.commit();
            f1(cast_for(group_exclusive{},type_policy::value_from(*p)));
            BOOST_UNORDERED_ADD_STATS(this->cstats.insertion,(pb.length()));
            return 1;
          }
          pg->mark_overflow(hash);
        }
      }
      else return -1;
    }
  }

  void rehash_if_full()
  {
    auto lck=exclusive_access();
    if(this->size_ctrl.size==this->size_ctrl.ml){
      this->unchecked_rehash_for_growth();
    }
  }

  template<typename GroupAccessMode,typename F>
  auto for_all_elements(GroupAccessMode access_mode,F f)const
    ->decltype(f(nullptr),void())
  {
    for_all_elements(
      access_mode,[&](group_type*,unsigned int,element_type* p){f(p);});
  }

  template<typename GroupAccessMode,typename F>
  auto for_all_elements(GroupAccessMode access_mode,F f)const
    ->decltype(f(nullptr,0,nullptr),void())
  {
    for_all_elements_while(
      access_mode,[&](group_type* pg,unsigned int n,element_type* p)
        {f(pg,n,p);return true;});
  }

  template<typename GroupAccessMode,typename F>
  auto for_all_elements_while(GroupAccessMode access_mode,F f)const
    ->decltype(f(nullptr),bool())
  {
    return for_all_elements_while(
      access_mode,[&](group_type*,unsigned int,element_type* p){return f(p);});
  }

  template<typename GroupAccessMode,typename F>
  auto for_all_elements_while(GroupAccessMode access_mode,F f)const
    ->decltype(f(nullptr,0,nullptr),bool())
  {
    auto p=this->arrays.elements();
    if(p){
      for(auto pg=this->arrays.groups(),last=pg+this->arrays.groups_size_mask+1;
          pg!=last;++pg,p+=N){
        auto lck=access(access_mode,(std::size_t)(pg-this->arrays.groups()));
        auto mask=this->match_really_occupied(pg,last);
        while(mask){
          auto n=unchecked_countr_zero(mask);
          if(!f(pg,n,p+n))return false;
          mask&=mask-1;
        }
      }
    }
    return true;
  }

#if defined(BOOST_UNORDERED_PARALLEL_ALGORITHMS)
  template<typename GroupAccessMode,typename ExecutionPolicy,typename F>
  auto for_all_elements(
    GroupAccessMode access_mode,ExecutionPolicy&& policy,F f)const
    ->decltype(f(nullptr),void())
  {
    for_all_elements(
      access_mode,std::forward<ExecutionPolicy>(policy),
      [&](group_type*,unsigned int,element_type* p){f(p);});
  }

  template<typename GroupAccessMode,typename ExecutionPolicy,typename F>
  auto for_all_elements(
    GroupAccessMode access_mode,ExecutionPolicy&& policy,F f)const
    ->decltype(f(nullptr,0,nullptr),void())
  {
    if(!this->arrays.elements())return;
    auto first=this->arrays.groups(),
         last=first+this->arrays.groups_size_mask+1;
    std::for_each(std::forward<ExecutionPolicy>(policy),first,last,
      [&,this](group_type& g){
        auto pos=static_cast<std::size_t>(&g-first);
        auto p=this->arrays.elements()+pos*N;
        auto lck=access(access_mode,pos);
        auto mask=this->match_really_occupied(&g,last);
        while(mask){
          auto n=unchecked_countr_zero(mask);
          f(&g,n,p+n);
          mask&=mask-1;
        }
      }
    );
  }

  template<typename GroupAccessMode,typename ExecutionPolicy,typename F>
  bool for_all_elements_while(
    GroupAccessMode access_mode,ExecutionPolicy&& policy,F f)const
  {
    if(!this->arrays.elements())return true;
    auto first=this->arrays.groups(),
         last=first+this->arrays.groups_size_mask+1;
    return std::all_of(std::forward<ExecutionPolicy>(policy),first,last,
      [&,this](group_type& g){
        auto pos=static_cast<std::size_t>(&g-first);
        auto p=this->arrays.elements()+pos*N;
        auto lck=access(access_mode,pos);
        auto mask=this->match_really_occupied(&g,last);
        while(mask){
          auto n=unchecked_countr_zero(mask);
          if(!f(p+n))return false;
          mask&=mask-1;
        }
        return true;
      }
    );
  }
#endif

  friend class boost::serialization::access;

  template<typename Archive>
  void serialize(Archive& ar,unsigned int version)
  {
    core::split_member(ar,*this,version);
  }

  template<typename Archive>
  void save(Archive& ar,unsigned int version)const
  {
    save(
      ar,version,
      std::integral_constant<bool,std::is_same<key_type,value_type>::value>{});
  }

  template<typename Archive>
  void save(Archive& ar,unsigned int,std::true_type /* set */)const
  {
    auto                                    lck=exclusive_access();
    const std::size_t                       s=super::size();
    const serialization_version<value_type> value_version;

    ar<<core::make_nvp("count",s);
    ar<<core::make_nvp("value_version",value_version);

    super::for_all_elements([&,this](element_type* p){
      auto& x=type_policy::value_from(*p);
      core::save_construct_data_adl(ar,std::addressof(x),value_version);
      ar<<serialization::make_nvp("item",x);
    });
  }

  template<typename Archive>
  void save(Archive& ar,unsigned int,std::false_type /* map */)const
  {
    using raw_key_type=typename std::remove_const<key_type>::type;
    using raw_mapped_type=typename std::remove_const<
      typename TypePolicy::mapped_type>::type;

    auto                                         lck=exclusive_access();
    const std::size_t                            s=super::size();
    const serialization_version<raw_key_type>    key_version;
    const serialization_version<raw_mapped_type> mapped_version;

    ar<<core::make_nvp("count",s);
    ar<<core::make_nvp("key_version",key_version);
    ar<<core::make_nvp("mapped_version",mapped_version);

    super::for_all_elements([&,this](element_type* p){
      /* To remain lib-independent from Boost.Serialization and not rely on
       * the user having included the serialization code for std::pair
       * (boost/serialization/utility.hpp), we serialize the key and the
       * mapped value separately.
       */

      auto& x=type_policy::value_from(*p);
      core::save_construct_data_adl(
        ar,std::addressof(x.first),key_version);
      ar<<serialization::make_nvp("key",x.first);
      core::save_construct_data_adl(
        ar,std::addressof(x.second),mapped_version);
      ar<<serialization::make_nvp("mapped",x.second);
    });
  }

  template<typename Archive>
  void load(Archive& ar,unsigned int version)
  {
    load(
      ar,version,
      std::integral_constant<bool,std::is_same<key_type,value_type>::value>{});
  }

  template<typename Archive>
  void load(Archive& ar,unsigned int,std::true_type /* set */)
  {
    auto                              lck=exclusive_access();
    std::size_t                       s;
    serialization_version<value_type> value_version;

    ar>>core::make_nvp("count",s);
    ar>>core::make_nvp("value_version",value_version);

    super::clear();
    super::reserve(s);

    for(std::size_t n=0;n<s;++n){
      archive_constructed<value_type> value("item",ar,value_version);
      auto&                           x=value.get();
      auto                            hash=this->hash_for(x);
      auto                            pos0=this->position_for(hash);

      if(this->find(x,pos0,hash))throw_exception(bad_archive_exception());
      auto loc=this->unchecked_emplace_at(pos0,hash,std::move(x));
      ar.reset_object_address(
        std::addressof(type_policy::value_from(*loc.p)),std::addressof(x));
    }
  }

  template<typename Archive>
  void load(Archive& ar,unsigned int,std::false_type /* map */)
  {
    using raw_key_type=typename std::remove_const<key_type>::type;
    using raw_mapped_type=typename std::remove_const<
      typename type_policy::mapped_type>::type;

    auto                                   lck=exclusive_access();
    std::size_t                            s;
    serialization_version<raw_key_type>    key_version;
    serialization_version<raw_mapped_type> mapped_version;

    ar>>core::make_nvp("count",s);
    ar>>core::make_nvp("key_version",key_version);
    ar>>core::make_nvp("mapped_version",mapped_version);

    super::clear();
    super::reserve(s);

    for(std::size_t n=0;n<s;++n){
      archive_constructed<raw_key_type>    key("key",ar,key_version);
      archive_constructed<raw_mapped_type> mapped("mapped",ar,mapped_version);
      auto&                                k=key.get();
      auto&                                m=mapped.get();
      auto                                 hash=this->hash_for(k);
      auto                                 pos0=this->position_for(hash);

      if(this->find(k,pos0,hash))throw_exception(bad_archive_exception());
      auto loc=this->unchecked_emplace_at(pos0,hash,std::move(k),std::move(m));
      ar.reset_object_address(
        std::addressof(type_policy::value_from(*loc.p).first),
        std::addressof(k));
      ar.reset_object_address(
        std::addressof(type_policy::value_from(*loc.p).second),
        std::addressof(m));
    }
  }

  static std::atomic<std::size_t> thread_counter;
  mutable multimutex_type         mutexes;
};

template<typename T,typename H,typename P,typename A>
std::atomic<std::size_t> concurrent_table<T,H,P,A>::thread_counter={};

#if defined(BOOST_MSVC)
#pragma warning(pop) /* C4714 */
#endif

#include <boost/unordered/detail/foa/restore_wshadow.hpp>

} /* namespace foa */
} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif
