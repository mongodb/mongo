//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_VECTOR_HPP
#define BOOST_CONTAINER_CONTAINER_VECTOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

// container
#include <boost/container/container_fwd.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/new_allocator.hpp> //new_allocator
#include <boost/container/throw_exception.hpp>
#include <boost/container/options.hpp>
// container detail
#include <boost/container/detail/advanced_insert_int.hpp>
#include <boost/container/detail/algorithm.hpp> //equal()
#include <boost/container/detail/alloc_helpers.hpp>
#include <boost/container/detail/allocation_type.hpp>
#include <boost/container/detail/copy_move_algo.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/detail/iterator.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/move/detail/iterator_to_raw_pointer.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/next_capacity.hpp>
#include <boost/container/detail/value_functors.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/version_type.hpp>
// intrusive
#include <boost/intrusive/pointer_traits.hpp>
// move
#include <boost/move/adl_move_swap.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/traits.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/move/detail/launder.hpp>
// move/detail
#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
#include <boost/move/detail/fwd_macros.hpp>
#endif
#include <boost/move/detail/move_helpers.hpp>
// move/algo
#include <boost/move/algo/adaptive_merge.hpp>
#include <boost/move/algo/unique.hpp>
#include <boost/move/algo/predicate.hpp>
#include <boost/move/algo/detail/set_difference.hpp>
// other
#include <boost/assert.hpp>
#include <boost/cstdint.hpp>

//std
#if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
#include <initializer_list>   //for std::initializer_list
#endif

namespace boost {
namespace container {

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED


template <class Pointer, bool IsConst>
class vec_iterator
{
   public:
   typedef std::random_access_iterator_tag                                          iterator_category;
   #ifdef BOOST_MOVE_CONTIGUOUS_ITERATOR_TAG
   typedef std::contiguous_iterator_tag                                             iterator_concept;
   #endif
   typedef typename boost::intrusive::pointer_traits<Pointer>::element_type         value_type;

   //Defining element_type to make libstdc++'s std::pointer_traits well-formed leads to ambiguity
   //due to LWG3446. So we need to specialize std::pointer_traits. See 
   //https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96416 for details. Many thanks to Jonathan Wakely
   //for explaining the issue.
   #ifndef BOOST_GNU_STDLIB
   //Define element_
   typedef typename boost::intrusive::pointer_traits<Pointer>::element_type         element_type;
   #endif
   typedef typename boost::intrusive::pointer_traits<Pointer>::difference_type      difference_type;
   typedef typename boost::intrusive::pointer_traits<Pointer>::size_type            size_type;
   typedef typename dtl::if_c
      < IsConst
      , typename boost::intrusive::pointer_traits<Pointer>::template
                                 rebind_pointer<const value_type>::type
      , Pointer
      >::type                                                                       pointer;
   typedef typename boost::intrusive::pointer_traits<pointer>                       ptr_traits;
   typedef typename ptr_traits::reference                                           reference;

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   Pointer m_ptr;

   class nat
   {
      public:
      Pointer get_ptr() const
      { return Pointer();  }
   };
   typedef typename dtl::if_c< IsConst
                             , vec_iterator<Pointer, false>
                             , nat>::type                                           nonconst_iterator;

   public:
   inline
      const Pointer &get_ptr() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return   m_ptr;  }

   inline
      Pointer &get_ptr() BOOST_NOEXCEPT_OR_NOTHROW
   {  return   m_ptr;  }

   inline explicit vec_iterator(Pointer ptr) BOOST_NOEXCEPT_OR_NOTHROW
      : m_ptr(ptr)
   {}
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:

   //Constructors
   inline vec_iterator() BOOST_NOEXCEPT_OR_NOTHROW
      : m_ptr()   //Value initialization to achieve "null iterators" (N3644)
   {}

   inline vec_iterator(const vec_iterator& other) BOOST_NOEXCEPT_OR_NOTHROW
      :  m_ptr(other.get_ptr())
   {}

   inline vec_iterator(const nonconst_iterator &other) BOOST_NOEXCEPT_OR_NOTHROW
      :  m_ptr(other.get_ptr())
   {}

   inline vec_iterator & operator=(const vec_iterator& other) BOOST_NOEXCEPT_OR_NOTHROW
   {  m_ptr = other.get_ptr();   return *this;  }

   //Pointer like operators
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reference operator*()   const BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr);  return *m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      pointer operator->()  const BOOST_NOEXCEPT_OR_NOTHROW
   {  return m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reference operator[](difference_type off) const BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr);  return m_ptr[off];  }

   //Increment / Decrement
   inline vec_iterator& operator++() BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr); ++m_ptr;  return *this; }

   inline vec_iterator operator++(int) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr); return vec_iterator(m_ptr++); }

   inline vec_iterator& operator--() BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr); --m_ptr; return *this;  }

   inline vec_iterator operator--(int) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(!!m_ptr); return vec_iterator(m_ptr--); }

   //Arithmetic
   inline vec_iterator& operator+=(difference_type off) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(m_ptr || !off); m_ptr += off; return *this;   }

   inline vec_iterator& operator-=(difference_type off) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(m_ptr || !off); m_ptr -= off; return *this;   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend vec_iterator operator+(const vec_iterator &x, difference_type off) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(x.m_ptr || !off); return vec_iterator(x.m_ptr+off);  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend vec_iterator operator+(difference_type off, vec_iterator right) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(right.m_ptr || !off); right.m_ptr += off;  return right; }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend vec_iterator operator-(vec_iterator left, difference_type off) BOOST_NOEXCEPT_OR_NOTHROW
   {  BOOST_ASSERT(left.m_ptr || !off); left.m_ptr -= off;  return left; }

   //Difference
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend difference_type operator-(const vec_iterator &left, const vec_iterator& right) BOOST_NOEXCEPT_OR_NOTHROW
   {  return left.m_ptr - right.m_ptr;   }

   //Comparison operators
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator==   (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr == r.m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator!=   (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr != r.m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator<    (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr < r.m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator<=   (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr <= r.m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>    (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr > r.m_ptr;  }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>=   (const vec_iterator& l, const vec_iterator& r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return l.m_ptr >= r.m_ptr;  }
};

template<class BiDirPosConstIt, class BiDirValueIt>
struct vector_insert_ordered_cursor
{
   typedef typename iterator_traits<BiDirPosConstIt>::value_type  size_type;
   typedef typename iterator_traits<BiDirValueIt>::reference      reference;

   inline vector_insert_ordered_cursor(BiDirPosConstIt posit, BiDirValueIt valueit)
      : last_position_it(posit), last_value_it(valueit)
   {}

   void operator --()
   {
      --last_value_it;
      --last_position_it;
      while(this->get_pos() == size_type(-1)){
         --last_value_it;
         --last_position_it;
      }
   }

   inline size_type get_pos() const
   {  return *last_position_it;  }

   inline reference get_val()
   {  return *last_value_it;  }

   BiDirPosConstIt last_position_it;
   BiDirValueIt last_value_it;
};

template<class Pointer, bool IsConst>
inline const Pointer &vector_iterator_get_ptr(const vec_iterator<Pointer, IsConst> &it) BOOST_NOEXCEPT_OR_NOTHROW
{  return   it.get_ptr();  }

template<class Pointer, bool IsConst>
inline Pointer &get_ptr(vec_iterator<Pointer, IsConst> &it) BOOST_NOEXCEPT_OR_NOTHROW
{  return  it.get_ptr();  }

struct initial_capacity_t {};

struct vector_uninitialized_size_t {};
BOOST_CONTAINER_CONSTANT_VAR vector_uninitialized_size_t vector_uninitialized_size = vector_uninitialized_size_t();

struct maybe_initial_capacity_t {};

template <class T>
struct vector_value_traits_base
{
   BOOST_STATIC_CONSTEXPR bool trivial_dctr = dtl::is_trivially_destructible<T>::value;
   BOOST_STATIC_CONSTEXPR bool trivial_dctr_after_move = has_trivial_destructor_after_move<T>::value;
};

template <class Allocator>
struct vector_value_traits
   : public vector_value_traits_base<typename Allocator::value_type>
{
   typedef vector_value_traits_base<typename Allocator::value_type> base_t;
   //This is the anti-exception array destructor
   //to deallocate values already constructed
   typedef typename dtl::if_c
      <base_t::trivial_dctr
      ,dtl::null_scoped_destructor_n<Allocator>
      ,dtl::scoped_destructor_n<Allocator>
      >::type   ArrayDestructor;
   //This is the anti-exception array deallocator
   typedef dtl::scoped_array_deallocator<Allocator> ArrayDeallocator;
};

//!This struct deallocates and allocated memory
template < class Allocator
         , class StoredSizeType
         , class AllocatorVersion = typename dtl::version<Allocator>::type
         >
struct vector_alloc_holder
   : public Allocator
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(vector_alloc_holder)

   public:
   typedef Allocator                                           allocator_type;
   typedef StoredSizeType                                      stored_size_type;
   typedef boost::container::allocator_traits<allocator_type>  allocator_traits_type;
   typedef typename allocator_traits_type::pointer             pointer;
   typedef typename allocator_traits_type::size_type           size_type;
   typedef typename allocator_traits_type::value_type          value_type;


   private:

   template<class SizeType>
   void do_initial_capacity(SizeType initial_capacity)
   {
      if (BOOST_UNLIKELY(initial_capacity > size_type(-1))) {
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");
      }
      else if (initial_capacity) {
         pointer reuse = pointer();
         size_type final_cap = static_cast<size_type>(initial_capacity);
         m_start = this->allocation_command(allocate_new, final_cap, final_cap, reuse);
         this->set_stored_capacity(final_cap);
      }
   }

   template<class SizeType>
   void do_maybe_initial_capacity(pointer p, SizeType initial_capacity)
   {
      if (BOOST_UNLIKELY(initial_capacity > size_type(-1))) {
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");
      }
      else if (p) {
         m_start = p;
      }
      else {
         BOOST_ASSERT(initial_capacity > 0);
         pointer reuse = pointer();
         size_type final_cap = static_cast<size_type>(initial_capacity);
         m_start = this->allocation_command(allocate_new, final_cap, final_cap, reuse);
         this->set_stored_capacity(final_cap);
      }
   }

   public:

   template <bool PropagateAllocator>
   inline static bool is_propagable_from(const allocator_type &from_alloc, pointer p, const allocator_type &to_alloc)
   {
      (void)p; (void)to_alloc; (void)from_alloc;
      const bool all_storage_propagable = !allocator_traits_type::is_partially_propagable::value ||
                                          !allocator_traits_type::storage_is_unpropagable(from_alloc, p);
      return all_storage_propagable &&
         (PropagateAllocator || allocator_traits_type::is_always_equal::value || allocator_traits_type::equal(from_alloc, to_alloc));
   }

   template <bool PropagateAllocator>
   inline static bool are_swap_propagable(const allocator_type &l_a, pointer l_p, const allocator_type &r_a, pointer r_p)
   {
      (void)l_p; (void)r_p; (void)l_a; (void)r_a;
      const bool all_storage_propagable = !allocator_traits_type::is_partially_propagable::value || 
              !(allocator_traits_type::storage_is_unpropagable(l_a, l_p) || allocator_traits_type::storage_is_unpropagable(r_a, r_p));
      return all_storage_propagable && (PropagateAllocator || allocator_traits_type::is_always_equal::value || allocator_traits_type::equal(l_a, r_a));
   }

   //Constructor, does not throw
   vector_alloc_holder()
      BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
      : allocator_type(), m_start(), m_size(), m_capacity()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   explicit vector_alloc_holder(BOOST_FWD_REF(AllocConvertible) a) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(boost::forward<AllocConvertible>(a)), m_start(), m_size(), m_capacity()
   {}


   template<class AllocConvertible, class SizeType>
   vector_alloc_holder(vector_uninitialized_size_t, BOOST_FWD_REF(AllocConvertible) a, SizeType initial_size)
      : allocator_type(boost::forward<AllocConvertible>(a))
      , m_start()
      //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_size(static_cast<stored_size_type>(initial_size))
      , m_capacity()
   {  this->do_initial_capacity(initial_size);  }

   template<class SizeType>
   vector_alloc_holder(vector_uninitialized_size_t, SizeType initial_size)
      : allocator_type()
      , m_start()
      //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_size(static_cast<stored_size_type>(initial_size))
      , m_capacity()
   {  this->do_initial_capacity(initial_size);  }

   vector_alloc_holder(initial_capacity_t, pointer p, size_type n)
      BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
      : allocator_type()
      , m_start(p)
      , m_size()
      //n is guaranteed to fit into stored_size_type
      , m_capacity(static_cast<stored_size_type>(n))
   {}

   template<class AllocFwd>
   vector_alloc_holder(initial_capacity_t, pointer p, size_type n, BOOST_FWD_REF(AllocFwd) a)
      : allocator_type(::boost::forward<AllocFwd>(a))
      , m_start(p)
      , m_size()
      , m_capacity(n)
   {}

   template<class AllocConvertible, class SizeType>
   vector_alloc_holder(maybe_initial_capacity_t, pointer p, SizeType initial_capacity, BOOST_FWD_REF(AllocConvertible) a)
      : allocator_type(boost::forward<AllocConvertible>(a))
      //, m_start()
      //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_size()
      , m_capacity(static_cast<stored_size_type>(initial_capacity))
   {  this->do_maybe_initial_capacity(p, initial_capacity);  }

   template<class SizeType>
   vector_alloc_holder(maybe_initial_capacity_t, pointer p, SizeType initial_capacity)
      : allocator_type()
      //, m_start()
      //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_size()
      , m_capacity(static_cast<stored_size_type>(initial_capacity))
   {  this->do_maybe_initial_capacity(p, initial_capacity);  }

   vector_alloc_holder(BOOST_RV_REF(vector_alloc_holder) holder) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(BOOST_MOVE_BASE(allocator_type, holder))
      , m_start(holder.m_start)
      , m_size(holder.m_size)
      , m_capacity(holder.m_capacity)
   {
      holder.m_start = pointer();
      holder.m_size = holder.m_capacity = 0;
   }

   inline ~vector_alloc_holder() BOOST_NOEXCEPT_OR_NOTHROW
   {
      if(this->m_capacity){
         this->deallocate(this->m_start, this->m_capacity);
      }
   }

   inline void set_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(s);   }

   inline void dec_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(this->m_size - s);   }

   inline void inc_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(this->m_size + s);   }

   inline void set_stored_capacity(size_type c) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_capacity = static_cast<stored_size_type>(c);  }

   inline pointer allocation_command(boost::container::allocation_type command,
                                 size_type limit_size, size_type &prefer_in_recvd_out_size, pointer &reuse)
   {
      typedef typename dtl::version<allocator_type>::type alloc_version;
      return this->priv_allocation_command(alloc_version(), command, limit_size, prefer_in_recvd_out_size, reuse);
   }

   inline pointer allocate(size_type n)
   {
      const size_type max_alloc = allocator_traits_type::max_size(this->alloc());
      const size_type max = max_alloc <= stored_size_type(-1) ? max_alloc : stored_size_type(-1);
      if (BOOST_UNLIKELY(max < n) )
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");

      return allocator_traits_type::allocate(this->alloc(), n);
   }

   inline void deallocate(const pointer &p, size_type n)
   {
      allocator_traits_type::deallocate(this->alloc(), p, n);
   }

   bool try_expand_fwd(size_type at_least)
   {
      //There is not enough memory, try to expand the old one
      const size_type new_cap = size_type(this->capacity() + at_least);
      size_type real_cap = new_cap;
      pointer reuse = this->start();
      bool const success = !!this->allocation_command(expand_fwd, new_cap, real_cap, reuse);
      //Check for forward expansion
      if(success){
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_expand_fwd;
         #endif
         this->capacity(real_cap);
      }
      return success;
   }

   template<class GrowthFactorType>
   size_type next_capacity(size_type additional_objects) const
   {
      BOOST_ASSERT(additional_objects > size_type(this->m_capacity - this->m_size));
      size_type max = allocator_traits_type::max_size(this->alloc());
      (clamp_by_stored_size_type<size_type>)(max, stored_size_type());
      const size_type remaining_cap = size_type(max - size_type(this->m_capacity));
      const size_type min_additional_cap = size_type(additional_objects - size_type(this->m_capacity - this->m_size));

      if ( remaining_cap < min_additional_cap )
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");

      return GrowthFactorType()( size_type(this->m_capacity), min_additional_cap, max);
   }

   pointer           m_start;
   stored_size_type  m_size;
   stored_size_type  m_capacity;

   void swap_resources(vector_alloc_holder &x) BOOST_NOEXCEPT_OR_NOTHROW
   {
      boost::adl_move_swap(this->m_start, x.m_start);
      boost::adl_move_swap(this->m_size, x.m_size);
      boost::adl_move_swap(this->m_capacity, x.m_capacity);
   }

   void steal_resources(vector_alloc_holder &x) BOOST_NOEXCEPT_OR_NOTHROW
   {
      this->m_start     = x.m_start;
      this->m_size      = x.m_size;
      this->m_capacity  = x.m_capacity;
      x.m_start = pointer();
      x.m_size = x.m_capacity = 0;
   }

   inline allocator_type &alloc() BOOST_NOEXCEPT_OR_NOTHROW
   {  return *this;  }

   inline const allocator_type &alloc() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return *this;  }

   inline pointer   start() const     BOOST_NOEXCEPT_OR_NOTHROW
      {  return m_start;  }
   inline       size_type capacity() const     BOOST_NOEXCEPT_OR_NOTHROW
      {  return m_capacity;  }
   inline void start(const pointer &p)       BOOST_NOEXCEPT_OR_NOTHROW
      {  m_start = p;  }
   inline void capacity(const size_type &c)  BOOST_NOEXCEPT_OR_NOTHROW
      {  BOOST_ASSERT( c <= stored_size_type(-1)); this->set_stored_capacity(c);  }

   static inline void on_capacity_overflow()
   { }

   private:
   void priv_first_allocation(size_type cap)
   {
      if(cap){
         pointer reuse = pointer();
         m_start = this->allocation_command(allocate_new, cap, cap, reuse);
         m_capacity = cap;
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_alloc;
         #endif
      }
   }

   pointer priv_allocation_command(version_1, boost::container::allocation_type command,
                         size_type limit_size,
                         size_type &prefer_in_recvd_out_size,
                         pointer &reuse)
   {
      (void)command;
      BOOST_ASSERT( (command & allocate_new));
      BOOST_ASSERT(!(command & nothrow_allocation));
      //First detect overflow on smaller stored_size_types
      if (BOOST_UNLIKELY(limit_size > stored_size_type(-1))){
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");
      }
      (clamp_by_stored_size_type<size_type>)(prefer_in_recvd_out_size, stored_size_type());
      pointer const p = this->allocate(prefer_in_recvd_out_size);
      reuse = pointer();
      return p;
   }

   pointer priv_allocation_command(version_2, boost::container::allocation_type command,
                         size_type limit_size,
                         size_type &prefer_in_recvd_out_size,
                         pointer &reuse)
   {
      //First detect overflow on smaller stored_size_types
      if (BOOST_UNLIKELY(limit_size > stored_size_type(-1))){
         boost::container::throw_length_error("get_next_capacity, allocator's max size reached");
      }
      (clamp_by_stored_size_type<size_type>)(prefer_in_recvd_out_size, stored_size_type());
      //Allocate memory 
      pointer p = this->alloc().allocation_command(command, limit_size, prefer_in_recvd_out_size, reuse);
      //If after allocation prefer_in_recvd_out_size is not representable by stored_size_type, truncate it.
      (clamp_by_stored_size_type<size_type>)(prefer_in_recvd_out_size, stored_size_type());
      return p;
   }
};

//!This struct deallocates and allocated memory
template <class Allocator, class StoredSizeType>
struct vector_alloc_holder<Allocator, StoredSizeType, version_0>
   : public Allocator
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(vector_alloc_holder)

   public:
   typedef Allocator                                     allocator_type;
   typedef boost::container::
      allocator_traits<allocator_type>                   allocator_traits_type;
   typedef typename allocator_traits_type::pointer       pointer;
   typedef typename allocator_traits_type::size_type     size_type;
   typedef typename allocator_traits_type::value_type    value_type;
   typedef StoredSizeType                                stored_size_type;
      
   template <class OtherAllocator, class OtherStoredSizeType, class OtherAllocatorVersion>
   friend struct vector_alloc_holder;

   //Constructor, does not throw
   vector_alloc_holder()
      BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
      : allocator_type(), m_size()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   explicit vector_alloc_holder(BOOST_FWD_REF(AllocConvertible) a) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(boost::forward<AllocConvertible>(a)), m_size()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   vector_alloc_holder(vector_uninitialized_size_t, BOOST_FWD_REF(AllocConvertible) a, size_type initial_size)
      : allocator_type(boost::forward<AllocConvertible>(a))
      , m_size(static_cast<stored_size_type>(initial_size))  //Size is initialized here...
   {
      //... and capacity here, so vector, must call uninitialized_xxx in the derived constructor
      this->priv_first_allocation(initial_size);
   }

   //Constructor, does not throw
   vector_alloc_holder(vector_uninitialized_size_t, size_type initial_size)
      : allocator_type()
      , m_size(static_cast<stored_size_type>(initial_size))  //Size is initialized here...
   {
      //... and capacity here, so vector, must call uninitialized_xxx in the derived constructor
      this->priv_first_allocation(initial_size);
   }

   vector_alloc_holder(BOOST_RV_REF(vector_alloc_holder) holder)
      : allocator_type(BOOST_MOVE_BASE(allocator_type, holder))
      , m_size(holder.m_size) //Size is initialized here so vector should only call uninitialized_xxx after this
   {
      ::boost::container::uninitialized_move_alloc_n
         (this->alloc(), boost::movelib::to_raw_pointer(holder.start()), m_size, boost::movelib::to_raw_pointer(this->start()));
      ::boost::container::destroy_alloc_n
         (this->alloc(), boost::movelib::to_raw_pointer(holder.start()), m_size);
      holder.m_size = 0;
   }

   template<class OtherAllocator, class OtherStoredSizeType, class OtherAllocatorVersion>
   vector_alloc_holder(BOOST_RV_REF_BEG vector_alloc_holder<OtherAllocator, OtherStoredSizeType, OtherAllocatorVersion> BOOST_RV_REF_END holder)
      : allocator_type()
      , m_size(holder.m_size) //Initialize it to m_size as first_allocation can only succeed or abort
   {
      //Different allocator type so we must check we have enough storage
      const size_type n = holder.m_size;
      this->priv_first_allocation(n);
      ::boost::container::uninitialized_move_alloc_n
         (this->alloc(), boost::movelib::to_raw_pointer(holder.start()), n, boost::movelib::to_raw_pointer(this->start()));
   }

   static inline void on_capacity_overflow()
   {  allocator_type::on_capacity_overflow();  }

   inline void set_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(s);   }

   inline void dec_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(this->m_size - s);   }

   inline void inc_stored_size(size_type s) BOOST_NOEXCEPT_OR_NOTHROW
      {  this->m_size = static_cast<stored_size_type>(this->m_size + s);   }

   inline void priv_first_allocation(size_type cap)
   {
      if(cap > allocator_type::internal_capacity){
         on_capacity_overflow();
      }
   }

   inline void deep_swap(vector_alloc_holder &x)
      {  this->priv_deep_swap(x);   }

   template<class OtherAllocator, class OtherStoredSizeType, class OtherAllocatorVersion>
   void deep_swap(vector_alloc_holder<OtherAllocator, OtherStoredSizeType, OtherAllocatorVersion> &x)
   {
      typedef typename real_allocator<value_type, OtherAllocator>::type other_allocator_type;
      if(this->m_size > other_allocator_type::internal_capacity || x.m_size > allocator_type::internal_capacity){
         on_capacity_overflow();
      }
      this->priv_deep_swap(x);
   }

   inline void swap_resources(vector_alloc_holder &) BOOST_NOEXCEPT_OR_NOTHROW
   {  //Containers with version 0 allocators can't be moved without moving elements one by one
      on_capacity_overflow();
   }

   inline void steal_resources(vector_alloc_holder &)
   {  //Containers with version 0 allocators can't be moved without moving elements one by one
      on_capacity_overflow();
   }

   inline allocator_type &alloc() BOOST_NOEXCEPT_OR_NOTHROW
   {  return *this;  }

   inline const allocator_type &alloc() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return *this;  }

   inline bool try_expand_fwd(size_type at_least)
   {  return !at_least;  }

   inline pointer start() const       BOOST_NOEXCEPT_OR_NOTHROW
   {  return allocator_type::internal_storage();  }
   
   inline size_type capacity() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return allocator_type::internal_capacity;  }
   
   stored_size_type m_size;

   private:

   template<class OtherAllocator, class OtherStoredSizeType, class OtherAllocatorVersion>
   void priv_deep_swap(vector_alloc_holder<OtherAllocator, OtherStoredSizeType, OtherAllocatorVersion> &x)
   {
      const size_type MaxTmpStorage = sizeof(value_type)*allocator_type::internal_capacity;
      value_type *const first_this = boost::movelib::to_raw_pointer(this->start());
      value_type *const first_x = boost::movelib::to_raw_pointer(x.start());

      if(this->m_size < x.m_size){
         boost::container::deep_swap_alloc_n<MaxTmpStorage>(this->alloc(), first_this, this->m_size, first_x, x.m_size);
      }
      else{
         boost::container::deep_swap_alloc_n<MaxTmpStorage>(this->alloc(), first_x, x.m_size, first_this, this->m_size);
      }
      boost::adl_move_swap(this->m_size, x.m_size);
   }
};

struct growth_factor_60;
struct growth_factor_100;

template<class Options, class AllocatorSizeType>
struct get_vector_opt
{
   typedef vector_opt< typename default_if_void<typename Options::growth_factor_type, growth_factor_60>::type
                     , typename default_if_void<typename Options::stored_size_type, AllocatorSizeType>::type
                     > type;
};

template<class AllocatorSizeType>
struct get_vector_opt<void, AllocatorSizeType>
{
   typedef vector_opt<growth_factor_60, AllocatorSizeType> type;
};

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//! A vector is a sequence that supports random access to elements, constant
//! time insertion and removal of elements at the end, and linear time insertion
//! and removal of elements at the beginning or in the middle. The number of
//! elements in a vector may vary dynamically; memory management is automatic.
//!
//! \tparam T The type of object that is stored in the vector
//! \tparam A The allocator used for all internal memory management, use void
//!   for the default allocator
//! \tparam Options A type produced from \c boost::container::vector_options.
template <class T, class A BOOST_CONTAINER_DOCONLY(= void), class Options BOOST_CONTAINER_DOCONLY(= void) >
class vector
{
public:
   //////////////////////////////////////////////
   //
   //                    types
   //
   //////////////////////////////////////////////
   typedef T                                                                           value_type;
   typedef BOOST_CONTAINER_IMPDEF
      (typename real_allocator<T BOOST_MOVE_I A>::type)                                allocator_type;
   typedef ::boost::container::allocator_traits<allocator_type>                        allocator_traits_t;
   typedef typename   allocator_traits<allocator_type>::pointer                        pointer;
   typedef typename   allocator_traits<allocator_type>::const_pointer                  const_pointer;
   typedef typename   allocator_traits<allocator_type>::reference                      reference;
   typedef typename   allocator_traits<allocator_type>::const_reference                const_reference;
   typedef typename   allocator_traits<allocator_type>::size_type                      size_type;
   typedef typename   allocator_traits<allocator_type>::difference_type                difference_type;
   typedef allocator_type                                                              stored_allocator_type;
   typedef BOOST_CONTAINER_IMPDEF(vec_iterator<pointer BOOST_MOVE_I false>)            iterator;
   typedef BOOST_CONTAINER_IMPDEF(vec_iterator<pointer BOOST_MOVE_I true >)            const_iterator;
   typedef BOOST_CONTAINER_IMPDEF(boost::container::reverse_iterator<iterator>)        reverse_iterator;
   typedef BOOST_CONTAINER_IMPDEF(boost::container::reverse_iterator<const_iterator>)  const_reverse_iterator;

private:
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   //`allocator_type::value_type` must match container's `value type`. If this
   //assertion fails, please review your allocator definition. 
   BOOST_CONTAINER_STATIC_ASSERT((dtl::is_same<value_type, typename allocator_traits_t::value_type>::value));

   typedef typename boost::container::
      allocator_traits<allocator_type>::size_type                             alloc_size_type;
   typedef typename get_vector_opt<Options, alloc_size_type>::type            options_type;
   typedef typename options_type::growth_factor_type                          growth_factor_type;
   typedef typename options_type::stored_size_type                            stored_size_type;
   typedef value_less<T>                                                      value_less_t;

   //If provided the stored_size option must specify a type that is equal or a type that is smaller.
   BOOST_CONTAINER_STATIC_ASSERT( (sizeof(stored_size_type) < sizeof(alloc_size_type) ||
                        dtl::is_same<stored_size_type, alloc_size_type>::value) );

   typedef typename dtl::version<allocator_type>::type alloc_version;
   typedef boost::container::vector_alloc_holder
      <allocator_type, stored_size_type> alloc_holder_t;

   alloc_holder_t m_holder;

   typedef allocator_traits<allocator_type>                      allocator_traits_type;
   template <class U, class UA, class UOptions>
   friend class vector;


   protected:
   template <bool PropagateAllocator>
   inline static bool is_propagable_from(const allocator_type &from_alloc, pointer p, const allocator_type &to_alloc)
   {  return alloc_holder_t::template is_propagable_from<PropagateAllocator>(from_alloc, p, to_alloc);  }

   template <bool PropagateAllocator>
   inline static bool are_swap_propagable( const allocator_type &l_a, pointer l_p
                                         , const allocator_type &r_a, pointer r_p)
   {  return alloc_holder_t::template are_swap_propagable<PropagateAllocator>(l_a, l_p, r_a, r_p);  }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(vector)
   typedef vector_value_traits<allocator_type> value_traits;
   typedef constant_iterator<T>            cvalue_iterator;

   protected:

   inline void steal_resources(vector &x)
   {  return this->m_holder.steal_resources(x.m_holder);   }

   inline void protected_set_size(size_type n)
   {  this->m_holder.m_size = static_cast<stored_size_type>(n);   }

   template<class AllocFwd>
   inline vector(initial_capacity_t, pointer initial_memory, size_type cap, BOOST_FWD_REF(AllocFwd) a)
      : m_holder(initial_capacity_t(), initial_memory, cap, ::boost::forward<AllocFwd>(a))
   {}

   template<class AllocFwd>
   inline vector(initial_capacity_t, pointer initial_memory, size_type cap, BOOST_FWD_REF(AllocFwd) a, vector &x)
      : m_holder(initial_capacity_t(), initial_memory, cap, ::boost::forward<AllocFwd>(a))
   {
      allocator_type &this_al = this->get_stored_allocator();
      if (this->template is_propagable_from<true>(x.get_stored_allocator(), x.data(), this_al)) {
         this->steal_resources(x);
      }
      else {
         const size_type sz = x.size();
         ::boost::container::uninitialized_move_alloc_n_source
            ( this_al, x.priv_raw_begin(), sz
            //Use launder to stop false positives from -Warray-bounds
            , boost::move_detail::launder(this->priv_raw_begin()));
         this->protected_set_size(sz);
         x.clear();
      }
   }

   inline vector(initial_capacity_t, pointer initial_memory, size_type cap)
      : m_holder(initial_capacity_t(), initial_memory, cap)
   {}

   template<class SizeType, class AllocFwd>
   inline vector(maybe_initial_capacity_t, pointer p, SizeType initial_capacity, BOOST_FWD_REF(AllocFwd) a)
      : m_holder(maybe_initial_capacity_t(), p, initial_capacity, ::boost::forward<AllocFwd>(a))
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += size_type(p != pointer());
      #endif
   }

   template<class SizeType>
   inline vector(maybe_initial_capacity_t, pointer p, SizeType initial_capacity)
      : m_holder(maybe_initial_capacity_t(), p, initial_capacity)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += size_type(p != pointer());
      #endif
   }

   template <class U>
   void protected_init_n(const size_type new_size, const U& u)
   {
      BOOST_ASSERT(this->empty());
      this->priv_resize_proxy(u).uninitialized_copy_n_and_update(this->m_holder.alloc(), this->priv_raw_begin(), new_size);
      this->m_holder.set_stored_size(new_size);
   }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Constructs a vector taking the allocator as parameter.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   vector() BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
      : m_holder()
   {}

   //! <b>Effects</b>: Constructs a vector taking the allocator as parameter.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   explicit vector(const allocator_type& a) BOOST_NOEXCEPT_OR_NOTHROW
      : m_holder(a)
   {}

   //! <b>Effects</b>: Constructs a vector and inserts n value initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's value initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   explicit vector(size_type n)
      :  m_holder(vector_uninitialized_size, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_value_init_alloc_n
         (this->m_holder.alloc(), n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n value initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's value initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   explicit vector(size_type n, const allocator_type &a)
      :  m_holder(vector_uninitialized_size, a, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_value_init_alloc_n
         (this->m_holder.alloc(), n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n default initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's default initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   //!
   //! <b>Note</b>: Non-standard extension
   vector(size_type n, default_init_t)
      :  m_holder(vector_uninitialized_size, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_default_init_alloc_n
         (this->m_holder.alloc(), n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n default initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's default initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   //!
   //! <b>Note</b>: Non-standard extension
   vector(size_type n, default_init_t, const allocator_type &a)
      :  m_holder(vector_uninitialized_size, a, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_default_init_alloc_n
         (this->m_holder.alloc(), n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   vector(size_type n, const T& value)
      :  m_holder(vector_uninitialized_size, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_fill_alloc_n
         (this->m_holder.alloc(), value, n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   vector(size_type n, const T& value, const allocator_type& a)
      :  m_holder(vector_uninitialized_size, a, n)
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += n != 0;
      #endif
      boost::container::uninitialized_fill_alloc_n
         (this->m_holder.alloc(), value, n, this->priv_raw_begin());
   }

   //! <b>Effects</b>: Constructs a vector
   //!   and inserts a copy of the range [first, last) in the vector.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's constructor taking a dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
//    template <class InIt>
//    vector(InIt first, InIt last
//           BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_c
//                                  < dtl::is_convertible<InIt BOOST_MOVE_I size_type>::value
//                                  BOOST_MOVE_I dtl::nat >::type * = 0)
//           ) -> vector<typename iterator_traits<InIt>::value_type, new_allocator<typename iterator_traits<InIt>::value_type>>;
   template <class InIt>
   vector(InIt first, InIt last
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_c
         < dtl::is_convertible<InIt BOOST_MOVE_I size_type>::value
         BOOST_MOVE_I dtl::nat >::type * = 0)
      )
      :  m_holder()
   {  this->assign(first, last); }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts a copy of the range [first, last) in the vector.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's constructor taking a dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InIt>
   vector(InIt first, InIt last, const allocator_type& a
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_c
         < dtl::is_convertible<InIt BOOST_MOVE_I size_type>::value
         BOOST_MOVE_I dtl::nat >::type * = 0)
      )
      :  m_holder(a)
   {  this->assign(first, last); }

   //! <b>Effects</b>: Copy constructs a vector.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocator_type's allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   vector(const vector &x)
      :  m_holder( vector_uninitialized_size
                 , allocator_traits_type::select_on_container_copy_construction(x.m_holder.alloc())
                 , x.size())
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += x.size() != 0;
      #endif
      ::boost::container::uninitialized_copy_alloc_n
         ( this->m_holder.alloc(), x.priv_raw_begin()
         , x.size(), this->priv_raw_begin());
   }

   //! <b>Effects</b>: Move constructor. Moves x's resources to *this.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   vector(BOOST_RV_REF(vector) x) BOOST_NOEXCEPT_OR_NOTHROW
      :  m_holder(boost::move(x.m_holder))
   {  BOOST_CONTAINER_STATIC_ASSERT((!allocator_traits_type::is_partially_propagable::value));  }

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!  and inserts a copy of the range [il.begin(), il.last()) in the vector
   //!
   //! <b>Throws</b>: If T's constructor taking a dereferenced initializer_list iterator throws.
   //!
   //! <b>Complexity</b>: Linear to the range [il.begin(), il.end()).
   vector(std::initializer_list<value_type> il, const allocator_type& a = allocator_type())
      :  m_holder(vector_uninitialized_size, a, il.size())
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += il.size() != 0;
      #endif
      ::boost::container::uninitialized_copy_alloc_n_source
         ( this->m_holder.alloc(), il.begin()
         , static_cast<size_type>(il.size()), this->priv_raw_begin());
   }
   #endif

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Move constructor. Moves x's resources to *this.
   //!
   //! <b>Throws</b>: If T's move constructor or allocation throws
   //!
   //! <b>Complexity</b>: Linear.
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherA>
   vector(BOOST_RV_REF_BEG vector<T, OtherA, Options> BOOST_RV_REF_END x
         , typename dtl::enable_if_c
            < dtl::is_version<typename real_allocator<T, OtherA>::type, 0>::value>::type * = 0
         )
      :  m_holder(boost::move(x.m_holder))
   {}

   #endif   // defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Copy constructs a vector using the specified allocator.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   vector(const vector &x, const allocator_type &a)
      :  m_holder(vector_uninitialized_size, a, x.size())
   {
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      this->num_alloc += x.size() != 0;
      #endif
      ::boost::container::uninitialized_copy_alloc_n_source
         ( this->m_holder.alloc(), x.priv_raw_begin()
         , x.size(), this->priv_raw_begin());
   }

   //! <b>Effects</b>: Move constructor using the specified allocator.
   //!                 Moves x's resources to *this if a == allocator_type().
   //!                 Otherwise copies values from x to *this.
   //!
   //! <b>Throws</b>: If allocation or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant if a == x.get_allocator(), linear otherwise.
   vector(BOOST_RV_REF(vector) x, const allocator_type &a)
      :  m_holder( vector_uninitialized_size, a
                 //In this allocator move constructor the allocator won't be propagated --v
                 , is_propagable_from<false>(x.get_stored_allocator(), x.m_holder.start(), a) ? 0 : x.size()
                 )
   {
      //In this allocator move constructor the allocator won't be propagated ---v
      if(is_propagable_from<false>(x.get_stored_allocator(), x.m_holder.start(), a)){
         this->m_holder.steal_resources(x.m_holder);
      }
      else{
         const size_type n = x.size();
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         this->num_alloc += n != 0;
         #endif
         ::boost::container::uninitialized_move_alloc_n_source
            ( this->m_holder.alloc(), x.priv_raw_begin()
            , n, this->priv_raw_begin());
      }
   }

   //! <b>Effects</b>: Destroys the vector. All stored values are destroyed
   //!   and used memory is deallocated.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements.
   ~vector() BOOST_NOEXCEPT_OR_NOTHROW
   {
      boost::container::destroy_alloc_n
         (this->get_stored_allocator(), this->priv_raw_begin(), this->m_holder.m_size);
      //vector_alloc_holder deallocates the data
   }

   //! <b>Effects</b>: Makes *this contain the same elements as x.
   //!
   //! <b>Postcondition</b>: this->size() == x.size(). *this contains a copy
   //! of each of x's elements.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in x.
   inline vector& operator=(BOOST_COPY_ASSIGN_REF(vector) x)
   {
      if (BOOST_LIKELY(&x != this)){
         this->priv_copy_assign(x);
      }
      return *this;
   }

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   //! <b>Effects</b>: Make *this container contains elements from il.
   //!
   //! <b>Complexity</b>: Linear to the range [il.begin(), il.end()).
   inline vector& operator=(std::initializer_list<value_type> il)
   {
      this->assign(il.begin(), il.end());
      return *this;
   }
   #endif

   //! <b>Effects</b>: Move assignment. All x's values are transferred to *this.
   //!
   //! <b>Postcondition</b>: x.empty(). *this contains a the elements x had
   //!   before the function.
   //!
   //! <b>Throws</b>: If allocator_traits_type::propagate_on_container_move_assignment
   //!   is false and (allocation throws or value_type's move constructor throws)
   //!
   //! <b>Complexity</b>: Constant if allocator_traits_type::
   //!   propagate_on_container_move_assignment is true or
   //!   this->get>allocator() == x.get_allocator(). Linear otherwise.
   inline vector& operator=(BOOST_RV_REF(vector) x)
      BOOST_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value
                        || allocator_traits_type::is_always_equal::value)
   {
      if (BOOST_LIKELY(&x != this)){
         this->priv_move_assign(boost::move(x));
      }
      return *this;
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Move assignment. All x's values are transferred to *this.
   //!
   //! <b>Postcondition</b>: x.empty(). *this contains a the elements x had
   //!   before the function.
   //!
   //! <b>Throws</b>: If move constructor/assignment of T throws or allocation throws
   //!
   //! <b>Complexity</b>: Linear.
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherA>
   inline typename dtl::enable_if_and
                           < vector&
                           , dtl::is_version<typename real_allocator<T, OtherA>::type, 0>
                           , dtl::is_different<typename real_allocator<T, OtherA>::type, allocator_type>
                           >::type
      operator=(BOOST_RV_REF_BEG vector<value_type, OtherA, Options> BOOST_RV_REF_END x)
   {
      this->priv_move_assign(boost::move(x));
      return *this;
   }

   //! <b>Effects</b>: Copy assignment. All x's values are copied to *this.
   //!
   //! <b>Postcondition</b>: x.empty(). *this contains a the elements x had
   //!   before the function.
   //!
   //! <b>Throws</b>: If move constructor/assignment of T throws or allocation throws
   //!
   //! <b>Complexity</b>: Linear.
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherA>
   inline typename dtl::enable_if_and
                           < vector&
                           , dtl::is_version<typename real_allocator<T, OtherA>::type, 0>
                           , dtl::is_different<typename real_allocator<T, OtherA>::type, allocator_type>
                           >::type
      operator=(const vector<value_type, OtherA, Options> &x)
   {
      this->priv_copy_assign(x);
      return *this;
   }

   #endif

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor/assignment or
   //!   T's constructor/assignment from dereferencing InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template <class InIt>
   void assign(InIt first, InIt last
      //Input iterators or version 0 allocator
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
         < void
         BOOST_MOVE_I dtl::is_convertible<InIt BOOST_MOVE_I size_type>
         BOOST_MOVE_I dtl::and_
            < dtl::is_different<alloc_version BOOST_MOVE_I version_0>
            BOOST_MOVE_I dtl::is_not_input_iterator<InIt>
            >
         >::type * = 0)
      )
   {
      //Overwrite all elements we can from [first, last)
      iterator cur = this->begin();
      const iterator end_it = this->end();
      for ( ; first != last && cur != end_it; ++cur, ++first){
         *cur = *first;
      }

      if (first == last){
         //There are no more elements in the sequence, erase remaining
         T* const end_pos = this->priv_raw_end();
         const size_type n = static_cast<size_type>(end_pos - boost::movelib::iterator_to_raw_pointer(cur));
         this->priv_destroy_last_n(n);
      }
      else{
         //There are more elements in the range, insert the remaining ones
         this->insert(this->cend(), first, last);
      }
   }

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   //! <b>Effects</b>: Assigns the the range [il.begin(), il.end()) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's constructor from dereferencing iniializer_list iterator throws.
   //!
   inline void assign(std::initializer_list<T> il)
   {
      this->assign(il.begin(), il.end());
   }
   #endif

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor/assignment or
   //!   T's constructor/assignment from dereferencing InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template <class FwdIt>
   void assign(FwdIt first, FwdIt last
      //Forward iterators and version > 0 allocator
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
         < void
         BOOST_MOVE_I dtl::is_same<alloc_version BOOST_MOVE_I version_0>
         BOOST_MOVE_I dtl::is_convertible<FwdIt BOOST_MOVE_I size_type>
         BOOST_MOVE_I dtl::is_input_iterator<FwdIt>
         >::type * = 0)
      )
   {
      typedef typename iter_size<FwdIt>::type it_size_type;
      //For Fwd iterators the standard only requires EmplaceConstructible and assignable from *first
      //so we can't do any backwards allocation
      const it_size_type sz = boost::container::iterator_udistance(first, last);
      if (BOOST_UNLIKELY(sz > size_type(-1))){
         boost::container::throw_length_error("vector::assign, FwdIt's max length reached");
      }

      const size_type input_sz = static_cast<size_type>(sz);
      const size_type old_capacity = this->capacity();
      if(input_sz > old_capacity){  //If input range is too big, we need to reallocate
         size_type real_cap = 0;
         pointer reuse(this->m_holder.start());
         pointer const ret(this->m_holder.allocation_command(allocate_new|expand_fwd, input_sz, real_cap = input_sz, reuse));
         if(!reuse){  //New allocation, just emplace new values
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_alloc;
            #endif
            pointer const old_p = this->m_holder.start();
            if(old_p){
               this->priv_destroy_all();
               this->m_holder.deallocate(old_p, old_capacity);
            }
            this->m_holder.start(ret);
            this->m_holder.capacity(real_cap);
            this->m_holder.m_size = 0;
            this->priv_uninitialized_construct_at_end(first, last);
            return;
         }
         else{
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_fwd;
            #endif
            this->m_holder.capacity(real_cap);
            //Forward expansion, use assignment + back deletion/construction that comes later
         }
      }

      boost::container::copy_assign_range_alloc_n(this->m_holder.alloc(), first, input_sz, this->priv_raw_begin(), this->size());
      m_holder.set_stored_size(input_sz);
   }

   //! <b>Effects</b>: Assigns the n copies of val to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   inline void assign(size_type n, const value_type& val)
   {  this->assign(cvalue_iterator(val, n), cvalue_iterator());   }

   //! <b>Effects</b>: Returns a copy of the internal allocator.
   //!
   //! <b>Throws</b>: If allocator's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline allocator_type get_allocator() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->m_holder.alloc();  }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline 
      stored_allocator_type &get_stored_allocator() BOOST_NOEXCEPT_OR_NOTHROW
   {  return this->m_holder.alloc(); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const stored_allocator_type &get_stored_allocator() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return this->m_holder.alloc(); }

   //////////////////////////////////////////////
   //
   //                iterators
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns an iterator to the first element contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline iterator begin() BOOST_NOEXCEPT_OR_NOTHROW
   { return iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_iterator begin() const BOOST_NOEXCEPT_OR_NOTHROW
   { return const_iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns an iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline iterator end() BOOST_NOEXCEPT_OR_NOTHROW
   {
      iterator it (this->m_holder.start());
      it += difference_type(this->m_holder.m_size);
      return it;  //Adding zero to null pointer is allowed (non-UB)
   }

   //! <b>Effects</b>: Returns a const_iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_iterator end() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->cend(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reverse_iterator rbegin() BOOST_NOEXCEPT_OR_NOTHROW
   { return reverse_iterator(this->end());      }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reverse_iterator rbegin() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->crbegin(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reverse_iterator rend() BOOST_NOEXCEPT_OR_NOTHROW
   { return reverse_iterator(this->begin());       }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reverse_iterator rend() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->crend(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_iterator cbegin() const BOOST_NOEXCEPT_OR_NOTHROW
   { return const_iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_iterator cend() const BOOST_NOEXCEPT_OR_NOTHROW
   {
      const_iterator it (this->m_holder.start());
      it += difference_type(this->m_holder.m_size);
      return it;  //Adding zero to null pointer is allowed (non-UB)
   }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reverse_iterator crbegin() const BOOST_NOEXCEPT_OR_NOTHROW
   { return const_reverse_iterator(this->end());}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reverse_iterator crend() const BOOST_NOEXCEPT_OR_NOTHROW
   { return const_reverse_iterator(this->begin()); }

   //////////////////////////////////////////////
   //
   //                capacity
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns true if the vector contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline bool empty() const BOOST_NOEXCEPT_OR_NOTHROW
   { return !this->m_holder.m_size; }

   //! <b>Effects</b>: Returns the number of the elements contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline size_type size() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->m_holder.m_size; }

   //! <b>Effects</b>: Returns the largest possible size of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline size_type max_size() const BOOST_NOEXCEPT_OR_NOTHROW
   { return allocator_traits_type::max_size(this->m_holder.alloc()); }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are value initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move or value initialization throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   inline void resize(size_type new_size)
   {  this->priv_resize(new_size, value_init, alloc_version());  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are default initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move or default initialization throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   //!
   //! <b>Note</b>: Non-standard extension
   inline void resize(size_type new_size, default_init_t)
   {  this->priv_resize(new_size, default_init, alloc_version());  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are copy constructed from x.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   inline void resize(size_type new_size, const T& x)
   {  this->priv_resize(new_size, x, alloc_version());  }

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline size_type capacity() const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->m_holder.capacity(); }

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //!
   //! <b>Throws</b>: If memory allocation allocation throws or T's copy/move constructor throws.
   inline void reserve(size_type new_cap)
   {
      if (this->capacity() < new_cap){
         this->priv_move_to_new_buffer(new_cap, alloc_version());
      }
   }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //!   with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   inline void shrink_to_fit()
   {  this->priv_shrink_to_fit(alloc_version());   }

   //////////////////////////////////////////////
   //
   //               element access
   //
   //////////////////////////////////////////////

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the first
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reference front() BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(!this->empty());
      return *this->m_holder.start();
   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reference front() const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(!this->empty());
      return *this->m_holder.start();
   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reference back() BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(!this->empty());
      return this->m_holder.start()[difference_type(this->m_holder.m_size - 1u)];
   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reference back()  const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(!this->empty());
      return this->m_holder.start()[this->m_holder.m_size - 1];
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reference operator[](size_type n) BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(this->m_holder.m_size > n);
      return this->m_holder.start()[difference_type(n)];
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reference operator[](size_type n) const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(this->m_holder.m_size > n);
      return this->m_holder.start()[n];
   }

   //! <b>Requires</b>: size() >= n.
   //!
   //! <b>Effects</b>: Returns an iterator to the nth element
   //!   from the beginning of the container. Returns end()
   //!   if n == size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator nth(size_type n) BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(this->m_holder.m_size >= n);
      return iterator(this->m_holder.start()+difference_type(n));
   }

   //! <b>Requires</b>: size() >= n.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the nth element
   //!   from the beginning of the container. Returns end()
   //!   if n == size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator nth(size_type n) const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(this->m_holder.m_size >= n);
      return const_iterator(this->m_holder.start()+difference_type(n));
   }

   //! <b>Requires</b>: begin() <= p <= end().
   //!
   //! <b>Effects</b>: Returns the index of the element pointed by p
   //!   and size() if p == end().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type index_of(iterator p) BOOST_NOEXCEPT_OR_NOTHROW
   {
      //Range check assert done in priv_index_of
      return this->priv_index_of(vector_iterator_get_ptr(p));
   }

   //! <b>Requires</b>: begin() <= p <= end().
   //!
   //! <b>Effects</b>: Returns the index of the element pointed by p
   //!   and size() if p == end().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type index_of(const_iterator p) const BOOST_NOEXCEPT_OR_NOTHROW
   {
      //Range check assert done in priv_index_of
      return this->priv_index_of(vector_iterator_get_ptr(p));
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline reference at(size_type n)
   {
      this->priv_throw_if_out_of_range(n);
      return this->m_holder.start()[difference_type(n)];
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const_reference at(size_type n) const
   {
      this->priv_throw_if_out_of_range(n);
      return this->m_holder.start()[n];
   }

   //////////////////////////////////////////////
   //
   //                 data access
   //
   //////////////////////////////////////////////

   //! <b>Returns</b>: A pointer such that [data(),data() + size()) is a valid range.
   //!   For a non-empty vector, data() == &front().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline T* data() BOOST_NOEXCEPT_OR_NOTHROW
   { return this->priv_raw_begin(); }

   //! <b>Returns</b>: A pointer such that [data(),data() + size()) is a valid range.
   //!   For a non-empty vector, data() == &front().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline const T * data()  const BOOST_NOEXCEPT_OR_NOTHROW
   { return this->priv_raw_begin(); }

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the vector.
   //!
   //! <b>Returns</b>: A reference to the created object.
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws or
   //!   T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   template<class ...Args>
   inline reference emplace_back(BOOST_FWD_REF(Args)...args)
   {
      T* const p = this->priv_raw_end();
      if (BOOST_LIKELY(this->room_enough())){
         //There is more memory, just construct a new object at the end
         allocator_traits_type::construct(this->m_holder.alloc(), p, ::boost::forward<Args>(args)...);
         ++this->m_holder.m_size;
         return *p;
      }
      else{
         typedef dtl::insert_emplace_proxy<allocator_type, Args...> proxy_t;
         return *this->priv_insert_forward_range_no_capacity
            (p, 1, proxy_t(::boost::forward<Args>(args)...), alloc_version());
      }
   }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the vector.
   //!
   //! <b>Throws</b>: If the in-place constructor throws.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Non-standard extension.
   template<class ...Args>
   inline bool stable_emplace_back(BOOST_FWD_REF(Args)...args)
   {
      const bool is_room_enough = this->room_enough() || (alloc_version::value == 2 && this->m_holder.try_expand_fwd(1u));
      if (BOOST_LIKELY(is_room_enough)){
         //There is more memory, just construct a new object at the end
         allocator_traits_type::construct(this->m_holder.alloc(), this->priv_raw_end(), ::boost::forward<Args>(args)...);
         ++this->m_holder.m_size;
      }
      return is_room_enough;
   }

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... before position
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws or
   //!   T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   template<class ...Args>
   inline iterator emplace(const_iterator position, BOOST_FWD_REF(Args) ...args)
   {
      BOOST_ASSERT(this->priv_in_range_or_end(position));
      //Just call more general insert(pos, size, value) and return iterator
      typedef dtl::insert_emplace_proxy<allocator_type, Args...> proxy_t;
      return this->priv_insert_forward_range( vector_iterator_get_ptr(position), 1
                                            , proxy_t(::boost::forward<Args>(args)...));
   }

   #else // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

   #define BOOST_CONTAINER_VECTOR_EMPLACE_CODE(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline reference emplace_back(BOOST_MOVE_UREF##N)\
   {\
      T* const p = this->priv_raw_end();\
      if (BOOST_LIKELY(this->room_enough())){\
         allocator_traits_type::construct (this->m_holder.alloc()\
            , this->priv_raw_end() BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         ++this->m_holder.m_size;\
         return *p;\
      }\
      else{\
         typedef dtl::insert_emplace_proxy_arg##N<allocator_type BOOST_MOVE_I##N BOOST_MOVE_TARG##N> proxy_t;\
         return *this->priv_insert_forward_range_no_capacity\
            ( p, 1, proxy_t(BOOST_MOVE_FWD##N), alloc_version());\
      }\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline bool stable_emplace_back(BOOST_MOVE_UREF##N)\
   {\
      const bool is_room_enough = this->room_enough() || (alloc_version::value == 2 && this->m_holder.try_expand_fwd(1u));\
      if (BOOST_LIKELY(is_room_enough)){\
         allocator_traits_type::construct (this->m_holder.alloc()\
            , this->priv_raw_end() BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         ++this->m_holder.m_size;\
      }\
      return is_room_enough;\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline iterator emplace(const_iterator pos BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      BOOST_ASSERT(this->priv_in_range_or_end(pos));\
      typedef dtl::insert_emplace_proxy_arg##N<allocator_type BOOST_MOVE_I##N BOOST_MOVE_TARG##N> proxy_t;\
      return this->priv_insert_forward_range(vector_iterator_get_ptr(pos), 1, proxy_t(BOOST_MOVE_FWD##N));\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_VECTOR_EMPLACE_CODE)
   #undef BOOST_CONTAINER_VECTOR_EMPLACE_CODE

   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x at the end of the vector.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(const T &x);

   //! <b>Effects</b>: Constructs a new element in the end of the vector
   //!   and moves the resources of x to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_back, T, void, priv_push_back)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of x before position.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, const T &x);

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a new element before position with x's resources.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, T, iterator, priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert n copies of x before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or p if n is 0.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   inline iterator insert(const_iterator p, size_type n, const T& x)
   {
      BOOST_ASSERT(this->priv_in_range_or_end(p));
      dtl::insert_n_copies_proxy<allocator_type> proxy(x);
      return this->priv_insert_forward_range(vector_iterator_get_ptr(p), n, proxy);
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or pos if first == last.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InpIt throws or T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to boost::container::iterator_distance [first, last).
   template <class InIt>
   iterator insert(const_iterator pos, InIt first, InIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename dtl::disable_if_or
         < void
         , dtl::is_convertible<InIt, size_type>
         , dtl::is_not_input_iterator<InIt>
         >::type * = 0
      #endif
      )
   {
      BOOST_ASSERT(this->priv_in_range_or_end(pos));
      const size_type n_pos = size_type(pos - this->cbegin());
      iterator it(vector_iterator_get_ptr(pos));
      for(;first != last; ++first){
         it = this->emplace(it, *first);
         ++it;
      }
      return iterator(this->m_holder.start() + difference_type(n_pos));
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class FwdIt>
   inline iterator insert(const_iterator pos, FwdIt first, FwdIt last
      , typename dtl::disable_if_or
         < void
         , dtl::is_convertible<FwdIt, size_type>
         , dtl::is_input_iterator<FwdIt>
         >::type * = 0
      )
   {
      typedef typename iter_size<FwdIt>::type it_size_type;
      BOOST_ASSERT(this->priv_in_range_or_end(pos));
      const it_size_type sz = boost::container::iterator_udistance(first, last);
      if (BOOST_UNLIKELY(sz > size_type(-1))){
         boost::container::throw_length_error("vector::insert, FwdIt's max length reached");
      }

      dtl::insert_range_proxy<allocator_type, FwdIt> proxy(first);
      return this->priv_insert_forward_range(vector_iterator_get_ptr(pos), static_cast<size_type>(sz), proxy);
   }
   #endif

   //! <b>Requires</b>: p must be a valid iterator of *this. num, must
   //!   be equal to boost::container::iterator_distance(first, last)
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or pos if first == last.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InpIt throws or T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to boost::container::iterator_distance [first, last).
   //!
   //! <b>Note</b>: This function avoids a linear operation to calculate boost::container::iterator_distance[first, last)
   //!   for forward and bidirectional iterators, and a one by one insertion for input iterators. This is a
   //!   a non-standard extension.
   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class InIt>
   inline iterator insert(const_iterator pos, size_type num, InIt first, InIt last)
   {
      BOOST_ASSERT(this->priv_in_range_or_end(pos));
      BOOST_ASSERT(dtl::is_input_iterator<InIt>::value ||
                   num == boost::container::iterator_udistance(first, last));
      (void)last;
      dtl::insert_range_proxy<allocator_type, InIt> proxy(first);
      return this->priv_insert_forward_range(vector_iterator_get_ptr(pos), num, proxy);
   }
   #endif

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of the [il.begin(), il.end()) range before position.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or position if first == last.
   //!
   //! <b>Complexity</b>: Linear to the range [il.begin(), il.end()).
   inline iterator insert(const_iterator position, std::initializer_list<value_type> il)
   {
      //Assertion done in insert()
      return this->insert(position, il.begin(), il.end());
   }
   #endif

   //! <b>Effects</b>: Removes the last element from the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   inline void pop_back() BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(!this->empty());
      //Destroy last element
      allocator_traits_type::destroy(this->get_stored_allocator(), this->priv_raw_end() - 1);
      --this->m_holder.m_size;
   }

   //! <b>Effects</b>: Erases the element at position pos.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements between pos and the
   //!   last element. Constant if pos is the last element.
   iterator erase(const_iterator position)
   {
      BOOST_ASSERT(this->priv_in_range(position));
      const pointer p = vector_iterator_get_ptr(position);
      T *const pos_ptr = boost::movelib::to_raw_pointer(p);
      T *const end_ptr = this->priv_raw_end();

      //Move elements forward and destroy last
      (void)::boost::container::move(pos_ptr + 1, end_ptr, pos_ptr);

      T *const last_ptr = end_ptr-1;
      if(!value_traits::trivial_dctr_after_move || pos_ptr == last_ptr){
         allocator_traits_type::destroy(this->get_stored_allocator(), last_ptr);
      }
      --this->m_holder.m_size;
      return iterator(p);
   }

   //! <b>Effects</b>: Erases the elements pointed by [first, last).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the distance between first and last
   //!   plus linear to the elements between pos and the last element.
   iterator erase(const_iterator first, const_iterator last)
   {
      BOOST_ASSERT(this->priv_in_range_or_end(first));
      BOOST_ASSERT(this->priv_in_range_or_end(last));
      BOOST_ASSERT(first <= last);
      if(first != last){
         T* const old_end_ptr = this->priv_raw_end();
         T* const first_ptr = boost::movelib::to_raw_pointer(vector_iterator_get_ptr(first));
         T* const last_ptr  = boost::movelib::to_raw_pointer(vector_iterator_get_ptr(last));
         T* const new_last_ptr = boost::movelib::to_raw_pointer(boost::container::move(last_ptr, old_end_ptr, first_ptr));
         const size_type n = static_cast<size_type>(old_end_ptr - new_last_ptr);
         if(!value_traits::trivial_dctr_after_move || old_end_ptr == last_ptr){
            boost::container::destroy_alloc_n(this->get_stored_allocator(), new_last_ptr, n);
         }
         this->m_holder.dec_stored_size(n);
      }
      return iterator(vector_iterator_get_ptr(first));
   }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline void swap(vector& x)
      BOOST_NOEXCEPT_IF( ((allocator_traits_type::propagate_on_container_swap::value
                                    || allocator_traits_type::is_always_equal::value) &&
                                    !dtl::is_version<allocator_type, 0>::value))
   {
      this->priv_swap(x, dtl::bool_<dtl::is_version<allocator_type, 0>::value>());
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherA>
   inline void swap(vector<T, OtherA, Options> & x
            , typename dtl::enable_if_and
                     < void
                     , dtl::is_version<typename real_allocator<T, OtherA>::type, 0>
                     , dtl::is_different<typename real_allocator<T, OtherA>::type, allocator_type>
                     >::type * = 0
            )
   {  this->m_holder.deep_swap(x.m_holder); }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //! <b>Effects</b>: Erases all the elements of the vector. Leaves the capacity() of the vector unchanged.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   inline void clear() BOOST_NOEXCEPT_OR_NOTHROW
   {  this->priv_destroy_all();  }

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline friend bool operator==(const vector& x, const vector& y)
   {  return x.size() == y.size() && ::boost::container::algo_equal(x.begin(), x.end(), y.begin());  }

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline friend bool operator!=(const vector& x, const vector& y)
   {  return !(x == y); }

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD friend bool operator<(const vector& x, const vector& y)
   {  return boost::container::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline friend bool operator>(const vector& x, const vector& y)
   {  return y < x;  }

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline friend bool operator<=(const vector& x, const vector& y)
   {  return !(y < x);  }

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline friend bool operator>=(const vector& x, const vector& y)
   {  return !(x < y);  }

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   inline friend void swap(vector& x, vector& y)
       BOOST_NOEXCEPT_IF(BOOST_NOEXCEPT(x.swap(y)))
   {  x.swap(y);  }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory
   //!   (memory expansion) that will not invalidate iterators.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //!
   //! <b>Throws</b>: If memory allocation allocation throws or T's copy/move constructor throws.
   //!
   //! <b>Note</b>: Non-standard extension.
   bool stable_reserve(size_type new_cap)
   {
      const size_type cp = this->capacity();
      return cp >= new_cap || (alloc_version::value == 2 && this->m_holder.try_expand_fwd(size_type(new_cap - cp)));
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   //Absolutely experimental. This function might change, disappear or simply crash!
   template<class BiDirPosConstIt, class BiDirValueIt>
   inline void insert_ordered_at(const size_type element_count, BiDirPosConstIt last_position_it, BiDirValueIt last_value_it)
   {
      typedef vector_insert_ordered_cursor<BiDirPosConstIt, BiDirValueIt> inserter_t;
      return this->priv_insert_ordered_at(element_count, inserter_t(last_position_it, last_value_it));
   }

   template<class InputIt>
   inline void merge(InputIt first, InputIt last)
   {  this->merge(first, last, value_less_t());  }

   template<class InputIt, class Compare>
   inline 
      typename dtl::enable_if_c
         < !dtl::is_input_iterator<InputIt>::value &&
           dtl::is_same<value_type, typename iterator_traits<InputIt>::value_type>::value
         , void>::type
      merge(InputIt first, InputIt last, Compare comp)
   {
      size_type const s = this->size();
      size_type const c = this->capacity();
      size_type n = 0;
      size_type const free_cap = c - s;
      //If not input iterator and new elements don't fit in the remaining capacity, merge in new buffer
      if(free_cap < (n = boost::container::iterator_udistance(first, last))){
         this->priv_merge_in_new_buffer(first, n, comp, alloc_version());
      }
      else{
         this->priv_merge_generic(first, last, comp);
      }
   }

   template<class InputIt, class Compare>
   inline 
      typename dtl::enable_if_c
         < dtl::is_input_iterator<InputIt>::value ||
           !dtl::is_same<value_type, typename iterator_traits<InputIt>::value_type>::value
         , void>::type
      merge(InputIt first, InputIt last, Compare comp)
   {
      this->priv_merge_generic(first, last, comp);
   }

   template<class InputIt>
   inline void merge_unique(InputIt first, InputIt last)
   {  this->merge_unique(first, last, value_less_t());  }

   template<class InputIt, class Compare>
   inline 
      typename dtl::enable_if_c
         < !dtl::is_input_iterator<InputIt>::value &&
           dtl::is_same<value_type, typename iterator_traits<InputIt>::value_type>::value
         , void>::type
      merge_unique(InputIt first, InputIt last, Compare comp)
   {
      size_type const old_size = this->size();
      this->priv_set_difference_back(first, last, comp);
      T *const raw_beg = this->priv_raw_begin();
      T *const raw_end = this->priv_raw_end();
      T *raw_pos = raw_beg + old_size;
      boost::movelib::adaptive_merge(raw_beg, raw_pos, raw_end, comp, raw_end, this->capacity() - this->size());
   }

   template<class InputIt, class Compare>
   inline 
      typename dtl::enable_if_c
         < dtl::is_input_iterator<InputIt>::value ||
           !dtl::is_same<value_type, typename iterator_traits<InputIt>::value_type>::value
         , void>::type
      merge_unique(InputIt first, InputIt last, Compare comp)
   {
      iterator pos = this->insert(this->end(), first, last);
      const iterator e = boost::movelib::inplace_set_unique_difference(pos, this->end(), this->begin(), pos, comp);
      this->erase(e, this->end());
      boost::movelib::adaptive_merge( this->begin(), pos, e, comp
                                    , this->priv_raw_end(), this->capacity() - this->size());
   }

   //Function for optimizations, not for users
   T *unused_storage(size_type &sz)
   {
      sz = static_cast<size_type>(this->capacity() - this->size());
      return this->priv_raw_end();
   }
  
   #endif

   private:
   template<class InputIt, class Compare>
   inline void priv_merge_generic(InputIt first, InputIt last, Compare comp)
   {
      size_type const old_s = this->size();
      this->insert(this->cend(), first, last);
      T* const raw_beg = this->priv_raw_begin();
      T* const raw_end = this->priv_raw_end();
      T* const raw_pos = raw_beg + old_s;
      boost::movelib::adaptive_merge(raw_beg, raw_pos, raw_end, comp, raw_end, this->capacity() - this->size());
   }

   template<class PositionValue>
   void priv_insert_ordered_at(const size_type element_count, PositionValue position_value)
   {
      const size_type old_size_pos = this->size();
      this->reserve(old_size_pos + element_count);
      T* const begin_ptr = this->priv_raw_begin();
      size_type insertions_left = element_count;
      size_type prev_pos = old_size_pos;
      size_type old_hole_size = element_count;

      //Exception rollback. If any copy throws before the hole is filled, values
      //already inserted/copied at the end of the buffer will be destroyed.
      typename value_traits::ArrayDestructor past_hole_values_destroyer
         (begin_ptr + old_size_pos + element_count, this->m_holder.alloc(), size_type(0u));
      //Loop for each insertion backwards, first moving the elements after the insertion point,
      //then inserting the element.
      while(insertions_left){
         --position_value;
         size_type const pos = position_value.get_pos();
         BOOST_ASSERT(pos != size_type(-1) && pos <= old_size_pos && pos <= prev_pos);
         //If needed shift the range after the insertion point and the previous insertion point.
         //Function will take care if the shift crosses the size() boundary, using copy/move
         //or uninitialized copy/move if necessary.
         size_type new_hole_size = (pos != prev_pos)
            ? priv_insert_ordered_at_shift_range(pos, prev_pos, this->size(), insertions_left)
            : old_hole_size
            ;
         if(new_hole_size){
            //The hole was reduced by priv_insert_ordered_at_shift_range so expand exception rollback range backwards
            past_hole_values_destroyer.increment_size_backwards(prev_pos - pos);
            //Insert the new value in the hole
            allocator_traits_type::construct(this->m_holder.alloc(), begin_ptr + pos + insertions_left - 1, position_value.get_val());
            if(--new_hole_size){
               //The hole was reduced by the new insertion by one
               past_hole_values_destroyer.increment_size_backwards(size_type(1u));
            }
            else{
               //Hole was just filled, disable exception rollback and change vector size
               past_hole_values_destroyer.release();
               this->m_holder.inc_stored_size(element_count);
            }
         }
         else{
            if(old_hole_size){
               //Hole was just filled by priv_insert_ordered_at_shift_range, disable exception rollback and change vector size
               past_hole_values_destroyer.release();
               this->m_holder.inc_stored_size(element_count);
            }
            //Insert the new value in the already constructed range
            begin_ptr[pos + insertions_left - 1] = position_value.get_val();
         }
         --insertions_left;
         old_hole_size = new_hole_size;
         prev_pos = pos;
      }
   }

   template<class InputIt, class Compare>
   void priv_set_difference_back(InputIt first1, InputIt last1, Compare comp)
   {
      T * old_first2 = this->priv_raw_begin();
      T * first2 = old_first2;
      T * last2  = this->priv_raw_end();

      while (first1 != last1) {
         if (first2 == last2){
            this->insert(this->cend(), first1, last1);
            return;
         }

         if (comp(*first1, *first2)) {
            this->emplace_back(*first1);
            T * const raw_begin = this->priv_raw_begin();
            if(old_first2 != raw_begin)
            {
               //Reallocation happened, update range
               first2 = raw_begin + (first2 - old_first2);
               last2  = raw_begin + (last2 - old_first2);
               old_first2 = raw_begin;
            }
            ++first1;
         }
         else {
            if (!comp(*first2, *first1)) {
               ++first1;
            }
            ++first2;
         }
      }
   }

   template<class FwdIt, class Compare>
   inline void priv_merge_in_new_buffer(FwdIt, size_type, Compare, version_0)
   {
      alloc_holder_t::on_capacity_overflow();
   }

   template<class FwdIt, class Compare, class Version>
   void priv_merge_in_new_buffer(FwdIt first, size_type n, Compare comp, Version)
   {
      size_type const new_size = this->size() + n;
      size_type new_cap = new_size;
      pointer p = pointer();
      pointer const new_storage = this->m_holder.allocation_command(allocate_new, new_size, new_cap, p);

      BOOST_ASSERT((new_cap >= this->size() ) && (new_cap - this->size()) >= n);
      allocator_type &a = this->m_holder.alloc();
      typename value_traits::ArrayDeallocator new_buffer_deallocator(new_storage, a, new_cap);
      typename value_traits::ArrayDestructor  new_values_destroyer(new_storage, a, 0u);
      T* pbeg  = this->priv_raw_begin();
      size_type const old_size = this->size();
      T* const pend = pbeg + old_size;
      T* d_first = boost::movelib::to_raw_pointer(new_storage);
      size_type added = n;
      //Merge in new buffer loop
      while(1){
         if(!n) {
            ::boost::container::uninitialized_move_alloc(this->m_holder.alloc(), pbeg, pend, d_first);
            break;
         } 
         else if(pbeg == pend) {
            ::boost::container::uninitialized_move_alloc_n(this->m_holder.alloc(), first, n, d_first);
            break;
         }
         //maintain stability moving external values only if they are strictly less
         else if(comp(*first, *pbeg)) {
            allocator_traits_type::construct( this->m_holder.alloc(), d_first, *first );
            new_values_destroyer.increment_size(1u);
            ++first;
            --n;
            ++d_first;
         }
         else{
            allocator_traits_type::construct( this->m_holder.alloc(), d_first, boost::move(*pbeg) );
            new_values_destroyer.increment_size(1u);
            ++pbeg;
            ++d_first;
         }
      }

      //Nothrow operations
      pointer const old_p     = this->m_holder.start();
      size_type const old_cap = this->m_holder.capacity();
      boost::container::destroy_alloc_n(a, boost::movelib::to_raw_pointer(old_p), old_size);
      if (old_cap > 0) {
         this->m_holder.deallocate(old_p, old_cap);
      }
      m_holder.set_stored_size(old_size + added);
      this->m_holder.start(new_storage);
      this->m_holder.capacity(new_cap);
      new_buffer_deallocator.release();
      new_values_destroyer.release();
   }

   inline bool room_enough() const
   {  return this->m_holder.m_size != this->m_holder.capacity();   }

   inline pointer back_ptr() const
   {  return this->m_holder.start() + difference_type(this->m_holder.m_size);  }

   inline size_type priv_index_of(pointer p) const
   {
      BOOST_ASSERT(this->m_holder.start() <= p);
      BOOST_ASSERT(p <= (this->m_holder.start()+difference_type(this->size())));
      return static_cast<size_type>(p - this->m_holder.start());
   }

   template<class OtherA>
   void priv_move_assign(BOOST_RV_REF_BEG vector<T, OtherA, Options> BOOST_RV_REF_END x
      , typename dtl::enable_if_c
         < dtl::is_version<typename real_allocator<T, OtherA>::type, 0>::value >::type * = 0)
   {
      if(!dtl::is_same<typename real_allocator<T, OtherA>::type, allocator_type>::value &&
          this->capacity() < x.size()){
         alloc_holder_t::on_capacity_overflow();
      }
      T* const this_start  = this->priv_raw_begin();
      T* const other_start = x.priv_raw_begin();
      const size_type this_sz  = m_holder.m_size;
      const size_type other_sz = static_cast<size_type>(x.m_holder.m_size);
      boost::container::move_assign_range_alloc_n(this->m_holder.alloc(), other_start, other_sz, this_start, this_sz);
      m_holder.set_stored_size(other_sz);
      //Not emptying the source container seems to be confusing for users as drop-in
      //replacement for non-static vectors, so clear it.
      x.clear();
   }

   template<class OtherA>
   void priv_move_assign_steal_or_assign(BOOST_RV_REF_BEG vector<T, OtherA, Options> BOOST_RV_REF_END x, dtl::true_type /*data_can_be_always_stolen*/)
   {
      this->clear();
      if (BOOST_LIKELY(!!this->m_holder.m_start))
         this->m_holder.deallocate(this->m_holder.m_start, this->m_holder.m_capacity);
      this->m_holder.steal_resources(x.m_holder);
   }

   template<class OtherA>
   void priv_move_assign_steal_or_assign(BOOST_RV_REF_BEG vector<T, OtherA, Options> BOOST_RV_REF_END x, dtl::false_type /*data_can_be_always_stolen*/)
   {
      const bool propagate_alloc = allocator_traits_type::propagate_on_container_move_assignment::value;
      allocator_type& this_alloc = this->m_holder.alloc();
      allocator_type& x_alloc = x.m_holder.alloc();

      //In this allocator move constructor the allocator might will be propagated, but to support small_vector-like
      //types, we need to check the currently owned buffers to know if they are propagable.
      const bool is_buffer_propagable_from_x = is_propagable_from<propagate_alloc>(x_alloc, x.m_holder.start(), this_alloc);

      if (is_buffer_propagable_from_x) {
         this->priv_move_assign_steal_or_assign(boost::move(x), dtl::true_type());
      }
      //Else do a one by one move. Also, clear the source as users find confusing
      //elements are still alive in the source container.
      else {
         this->assign( boost::make_move_iterator(boost::movelib::iterator_to_raw_pointer(x.begin()))
                     , boost::make_move_iterator(boost::movelib::iterator_to_raw_pointer(x.end()))   );
         x.clear();
      }
   }

   template<class OtherA>
   void priv_move_assign(BOOST_RV_REF_BEG vector<T, OtherA, Options> BOOST_RV_REF_END x
      , typename dtl::disable_if_or
         < void
         , dtl::is_version<typename real_allocator<T, OtherA>::type, 0>
         , dtl::is_different<typename real_allocator<T, OtherA>::type, allocator_type>
         >::type * = 0)
   {
      //for move assignment, no aliasing (&x != this) is assumed.
      //x.size() == 0 is allowed for buggy std libraries.
      BOOST_ASSERT(this != &x || x.size() == 0);
      const bool alloc_is_always_equal = allocator_traits_type::is_always_equal::value;
      const bool propagate_alloc = allocator_traits_type::propagate_on_container_move_assignment::value;
      const bool partially_propagable_alloc = allocator_traits_type::is_partially_propagable::value;
      const bool data_can_be_always_be_stolen = alloc_is_always_equal || (propagate_alloc && !partially_propagable_alloc);

      this->priv_move_assign_steal_or_assign(boost::move(x), dtl::bool_<data_can_be_always_be_stolen>());

      //Move allocator if needed
      allocator_type& this_alloc = this->m_holder.alloc();
      allocator_type& x_alloc    = x.m_holder.alloc();
      dtl::move_alloc(this_alloc, x_alloc, dtl::bool_<propagate_alloc>());
   }

   template<class OtherA>
   void priv_copy_assign(const vector<T, OtherA, Options> &x
      , typename dtl::enable_if_c
         < dtl::is_version<typename real_allocator<T, OtherA>::type, 0>::value >::type * = 0)
   {
      if(!dtl::is_same<typename real_allocator<T, OtherA>::type, allocator_type>::value &&
         this->capacity() < x.size()){
         alloc_holder_t::on_capacity_overflow();
      }
      T* const this_start  = this->priv_raw_begin();
      T* const other_start = x.priv_raw_begin();
      const size_type this_sz  = m_holder.m_size;
      const size_type other_sz = static_cast<size_type>(x.m_holder.m_size);
      boost::container::copy_assign_range_alloc_n(this->m_holder.alloc(), other_start, other_sz, this_start, this_sz);
      m_holder.set_stored_size(other_sz);
   }

   template<class OtherA>
   typename dtl::disable_if_or
      < void
      , dtl::is_version<typename real_allocator<T, OtherA>::type, 0>
      , dtl::is_different<typename real_allocator<T, OtherA>::type, allocator_type>
      >::type
      priv_copy_assign(const vector<T, OtherA, Options> &x)
   {
      allocator_type &this_alloc     = this->m_holder.alloc();
      const allocator_type &x_alloc  = x.m_holder.alloc();
      dtl::bool_<allocator_traits_type::
         propagate_on_container_copy_assignment::value> flag;
      if(flag && this_alloc != x_alloc){
         this->clear();
         this->shrink_to_fit();
      }
      dtl::assign_alloc(this_alloc, x_alloc, flag);
      this->assign( x.priv_raw_begin(), x.priv_raw_end() );
   }

   template<class Vector>  //Template it to avoid it in explicit instantiations
   inline void priv_swap(Vector &x, dtl::true_type)   //version_0
   {  this->m_holder.deep_swap(x.m_holder);  }

   template<class Vector>  //Template it to avoid it in explicit instantiations
   void priv_swap(Vector &x, dtl::false_type)  //version_N
   {
      BOOST_ASSERT(allocator_traits_type::propagate_on_container_swap::value ||
                   allocator_traits_type::is_always_equal::value ||
                   this->get_stored_allocator() == x.get_stored_allocator());

      if (BOOST_UNLIKELY(&x == this)) {
         return;
      }

      //Just swap internals
      this->m_holder.swap_resources(x.m_holder);
      //And now swap the allocator
      dtl::bool_<allocator_traits_type::propagate_on_container_swap::value> flag;
      dtl::swap_alloc(this->m_holder.alloc(), x.m_holder.alloc(), flag);
   }

   protected:
   template<class Vector>  //Template it to avoid it in explicit instantiations
   void prot_swap_small(Vector &x, std::size_t internal_capacity)  //version_N
   {
      if (BOOST_UNLIKELY(&x == this)){
         return;
      }

      const bool propagate_alloc = allocator_traits_type::propagate_on_container_swap::value;
      if(are_swap_propagable<propagate_alloc>
         ( this->get_stored_allocator(), this->m_holder.start(), x.get_stored_allocator(), x.m_holder.start())){
         this->priv_swap(x, dtl::false_());
         return;
      }

      allocator_type &th_al = this->get_stored_allocator();
      allocator_type &ot_al = x.get_stored_allocator();

      const bool is_this_data_propagable = is_propagable_from<propagate_alloc>(th_al, this->data(), ot_al);
      const bool is_that_data_propagable = is_propagable_from<propagate_alloc>(ot_al, x.data(), th_al);

      if(internal_capacity && (is_this_data_propagable || is_that_data_propagable)) {
         //steal memory from src to dst, but move elements from dst to src
         vector& extmem = is_this_data_propagable ? *this : x;
         vector& intmem = is_this_data_propagable ? x : *this;

         //Reset extmem to the internal storage and backup data
         pointer const orig_extdata = extmem.data();
         const size_type orig_extmem_size = extmem.size();
         const size_type orig_extmem_cap = extmem.capacity();

         //New safe state for extmem -> empty, internal storage
         extmem.m_holder.m_start = extmem.get_stored_allocator().internal_storage();
         extmem.m_holder.set_stored_size(0u);
         extmem.m_holder.set_stored_capacity(internal_capacity);

         {
            //Deallocate on exception
            typename value_traits::ArrayDeallocator new_buffer_deallocator(orig_extdata, extmem.get_stored_allocator(), orig_extmem_cap);
            typename value_traits::ArrayDestructor  new_values_destroyer(orig_extdata, extmem.get_stored_allocator(), orig_extmem_size);

            //Move internal memory data to the internal memory data of the target, this can throw
            BOOST_ASSERT(extmem.capacity() >= intmem.size());
            ::boost::container::uninitialized_move_alloc_n
               (intmem.get_stored_allocator(), intmem.priv_raw_begin(), intmem.size(), extmem.priv_raw_begin());

            //Exception not thrown, commit new state
            extmem.m_holder.set_stored_size(intmem.size());
            //Throwing part passed, disable rollback
            new_buffer_deallocator.release();
            new_values_destroyer.release();
         }

         //Destroy moved elements from intmem
         boost::container::destroy_alloc_n
            ( intmem.get_stored_allocator(), intmem.priv_raw_begin()
            , intmem.size());

         //Adopt dynamic buffer
         intmem.m_holder.m_start = orig_extdata;
         intmem.m_holder.set_stored_size(orig_extmem_size);
         intmem.m_holder.set_stored_capacity(orig_extmem_cap);

         //And now swap the allocator
         dtl::swap_alloc(this->m_holder.alloc(), x.m_holder.alloc(), dtl::bool_<propagate_alloc>());
      }
      else {   //swap element by element and insert rest
         bool const t_smaller = this->size() < x.size();
         vector &sml = t_smaller ? *this : x;
         vector &big = t_smaller ? x : *this;

         //swap element by element until common size
         size_type const common_elements = sml.size();
         for(size_type i = 0; i != common_elements; ++i){
            boost::adl_move_swap(sml[i], big[i]);
         }

         //And now swap the allocator to be able to construct new elements in sml with the proper allocator
         dtl::swap_alloc(this->m_holder.alloc(), x.m_holder.alloc(), dtl::bool_<propagate_alloc>());

         //move-insert the remaining range
         T *const raw_big_nth = boost::movelib::iterator_to_raw_pointer(big.nth(common_elements));
         sml.insert(sml.cend()
            , boost::make_move_iterator(raw_big_nth)
            , boost::make_move_iterator(boost::movelib::iterator_to_raw_pointer(big.end())));

         //Destroy remaining, moved, elements with their original allocator
         boost::container::destroy_alloc_n
            ( sml.get_stored_allocator(), raw_big_nth
            , std::size_t(big.m_holder.m_size - common_elements));
         big.m_holder.set_stored_size(common_elements);
      }
   }
   private:
   inline void priv_move_to_new_buffer(size_type, version_0)
   {  alloc_holder_t::on_capacity_overflow();  }

   inline dtl::insert_range_proxy<allocator_type, boost::move_iterator<T*> > priv_dummy_empty_proxy()
   {
      return dtl::insert_range_proxy<allocator_type, boost::move_iterator<T*> >
         (::boost::make_move_iterator((T *)0));
   }

   inline void priv_move_to_new_buffer(size_type new_cap, version_1)
   {
      //There is not enough memory, allocate a new buffer
      //Pass the hint so that allocators can take advantage of this.
      pointer const p = this->m_holder.allocate(new_cap);
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      ++this->num_alloc;
      #endif
      //We will reuse insert code, so create a dummy input iterator
      this->priv_insert_forward_range_new_allocation
         ( boost::movelib::to_raw_pointer(p), new_cap, this->priv_raw_end(), 0, this->priv_dummy_empty_proxy());
   }

   void priv_move_to_new_buffer(size_type new_cap, version_2)
   {
      //There is not enough memory, allocate a new
      //buffer or expand the old one.
      bool same_buffer_start;
      size_type real_cap = 0;
      pointer reuse(this->m_holder.start());
      pointer const ret(this->m_holder.allocation_command(allocate_new | expand_fwd | expand_bwd, new_cap, real_cap = new_cap, reuse));

      //Check for forward expansion
      same_buffer_start = reuse && this->m_holder.start() == ret;
      if(same_buffer_start){
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_expand_fwd;
         #endif
         this->m_holder.capacity(real_cap);
      }
      else{ //If there is no forward expansion, move objects, we will reuse insertion code
         T * const new_mem = boost::movelib::to_raw_pointer(ret);
         T * const ins_pos = this->priv_raw_end();
         if(reuse){   //Backwards (and possibly forward) expansion
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_bwd;
            #endif
            this->priv_insert_forward_range_expand_backwards
               ( new_mem, real_cap, ins_pos, 0, this->priv_dummy_empty_proxy());
         }
         else{ //New buffer
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_alloc;
            #endif
            this->priv_insert_forward_range_new_allocation
               ( new_mem, real_cap, ins_pos, 0, this->priv_dummy_empty_proxy());
         }
      }
   }

   void priv_destroy_last_n(const size_type n) BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(n <= this->m_holder.m_size);
      boost::container::destroy_alloc_n(this->get_stored_allocator(), this->priv_raw_end() - n, n);
      this->m_holder.dec_stored_size(n);
   }

   template<class InpIt>
   void priv_uninitialized_construct_at_end(InpIt first, InpIt last)
   {
      T* const old_end_pos = this->priv_raw_end();
      T* const new_end_pos = boost::container::uninitialized_copy_alloc(this->m_holder.alloc(), first, last, old_end_pos);
      this->m_holder.inc_stored_size(static_cast<size_type>(new_end_pos - old_end_pos));
   }

   void priv_destroy_all() BOOST_NOEXCEPT_OR_NOTHROW
   {
      boost::container::destroy_alloc_n
         (this->get_stored_allocator(), this->priv_raw_begin(), this->m_holder.m_size);
      this->m_holder.m_size = 0;
   }

   template<class U>
   inline iterator priv_insert(const const_iterator &p, BOOST_FWD_REF(U) u)
   {
      return this->emplace(p, ::boost::forward<U>(u));
   }

   template <class U>
   inline void priv_push_back(BOOST_FWD_REF(U) u)
   {
      this->emplace_back(::boost::forward<U>(u));
   }

   //Overload to support compiler errors that instantiate too much
   inline void priv_push_back(::boost::move_detail::nat)
   {}

   inline iterator priv_insert(const_iterator, ::boost::move_detail::nat)
   {  return iterator();  }

   inline dtl::insert_n_copies_proxy<allocator_type> priv_resize_proxy(const T &x)
   {  return dtl::insert_n_copies_proxy<allocator_type>(x);   }

   inline dtl::insert_default_initialized_n_proxy<allocator_type> priv_resize_proxy(default_init_t)
   {  return dtl::insert_default_initialized_n_proxy<allocator_type>();  }

   inline dtl::insert_value_initialized_n_proxy<allocator_type> priv_resize_proxy(value_init_t)
   {  return dtl::insert_value_initialized_n_proxy<allocator_type>(); }

   protected:
   void prot_shrink_to_fit_small(pointer const small_buffer, const size_type small_capacity)
   {
      const size_type cp = this->m_holder.capacity();
      if (cp && this->m_holder.m_start != small_buffer) {   //Do something only if a dynamic buffer is used
         const size_type sz = this->size();
         if (!sz) {
            if (BOOST_LIKELY(!!this->m_holder.m_start))
               this->m_holder.deallocate(this->m_holder.m_start, cp);
            this->m_holder.m_start = small_buffer;
            this->m_holder.set_stored_capacity(small_capacity);
         }
         else if(sz <= small_capacity) {
            T *const oldbuf = boost::movelib::to_raw_pointer(this->m_holder.m_start);
            ::boost::container::uninitialized_move_alloc_n
               ( this->get_stored_allocator()
               , oldbuf
               , sz
               , boost::movelib::to_raw_pointer(small_buffer)
               );
            boost::container::destroy_alloc_n(this->get_stored_allocator(), oldbuf, sz);

            if (BOOST_LIKELY(!!this->m_holder.m_start))
               this->m_holder.deallocate(this->m_holder.m_start, cp);

            this->m_holder.m_start = small_buffer;
            this->m_holder.set_stored_capacity(small_capacity);
         }
         else if (sz < cp) {
            this->priv_move_to_new_buffer(sz, alloc_version());
         }
      }
   }

   private:
   inline void priv_shrink_to_fit(version_0) BOOST_NOEXCEPT_OR_NOTHROW
   {}

   void priv_shrink_to_fit(version_1)
   {
      const size_type cp = this->m_holder.capacity();
      if(cp){
         const size_type sz = this->size();
         if(!sz){
            if(BOOST_LIKELY(!!this->m_holder.m_start))
               this->m_holder.deallocate(this->m_holder.m_start, cp);
            this->m_holder.m_start     = pointer();
            this->m_holder.m_capacity  = 0;
         }
         else if(sz < cp){
            this->priv_move_to_new_buffer(sz, alloc_version());
         }
      }
   }

   void priv_shrink_to_fit(version_2) BOOST_NOEXCEPT_OR_NOTHROW
   {
      const size_type cp = this->m_holder.capacity();
      if(cp){
         const size_type sz = this->size();
         if(!sz){
            if(BOOST_LIKELY(!!this->m_holder.m_start))
               this->m_holder.deallocate(this->m_holder.m_start, cp);
            this->m_holder.m_start     = pointer();
            this->m_holder.m_capacity  = 0;
         }
         else{
            size_type received_size = sz;
            pointer reuse(this->m_holder.start());
            if(this->m_holder.allocation_command
               (shrink_in_place | nothrow_allocation, cp, received_size, reuse)){
               this->m_holder.capacity(received_size);
               #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
               ++this->num_shrink;
               #endif
            }
         }
      }
   }

   #ifdef _MSC_VER
   #pragma warning (push)
   #pragma warning(disable: 4702)   //Disable unreachable code warning
   #endif
   template <class InsertionProxy>
   inline iterator priv_insert_forward_range_no_capacity
      (T * const, const size_type, const InsertionProxy , version_0)
   {
      return alloc_holder_t::on_capacity_overflow(), iterator();
   }
   #ifdef _MSC_VER
   #pragma warning (pop)
   #endif

   template <class InsertionProxy>
   BOOST_CONTAINER_NOINLINE iterator priv_insert_forward_range_no_capacity
      (T *const raw_pos, const size_type n, const InsertionProxy insert_range_proxy, version_1)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type n_pos = static_cast<size_type>(raw_pos - this->priv_raw_begin());

      const size_type new_cap = this->m_holder.template next_capacity<growth_factor_type>(n);
      //Pass the hint so that allocators can take advantage of this.
      T * const new_buf = boost::movelib::to_raw_pointer(this->m_holder.allocate(new_cap));
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      ++this->num_alloc;
      #endif
      this->priv_insert_forward_range_new_allocation(new_buf, new_cap, raw_pos, n, insert_range_proxy);
      return iterator(this->m_holder.start() + difference_type(n_pos));
   }

   template <class InsertionProxy>
   BOOST_CONTAINER_NOINLINE iterator priv_insert_forward_range_no_capacity
      (T *const raw_pos, const size_type n, const InsertionProxy insert_range_proxy, version_2)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type n_pos = size_type(raw_pos - this->priv_raw_begin());

      //There is not enough memory, allocate a new
      //buffer or expand the old one.
      size_type real_cap = this->m_holder.template next_capacity<growth_factor_type>(n);
      pointer reuse(this->m_holder.start());
      pointer const ret (this->m_holder.allocation_command
         (allocate_new | expand_fwd | expand_bwd, size_type(this->m_holder.m_size + n), real_cap, reuse));

      //Buffer reallocated
      if(reuse){
         //Forward expansion, delay insertion
         if(this->m_holder.start() == ret){
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_fwd;
            #endif
            this->m_holder.capacity(real_cap);
            //Expand forward
            this->priv_insert_forward_range_expand_forward
               (raw_pos, n, insert_range_proxy, dtl::bool_<dtl::is_single_value_proxy<InsertionProxy>::value>());
         }
         //Backwards (and possibly forward) expansion
         else{
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_bwd;
            #endif
            this->priv_insert_forward_range_expand_backwards
               (boost::movelib::to_raw_pointer(ret), real_cap, raw_pos, n, insert_range_proxy);
         }
      }
      //New buffer
      else{
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_alloc;
         #endif
         this->priv_insert_forward_range_new_allocation
            ( boost::movelib::to_raw_pointer(ret), real_cap, raw_pos, n, insert_range_proxy);
      }

      return iterator(this->m_holder.start() + (difference_type)(n_pos));
   }

   template <class InsertionProxy>
   inline iterator priv_insert_forward_range
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy)
   {
      BOOST_ASSERT(this->m_holder.capacity() >= this->m_holder.m_size);
      T *const p = boost::movelib::to_raw_pointer(pos);
      //Check if we have enough memory or try to expand current memory
      if (BOOST_LIKELY(n <= (this->m_holder.capacity() - this->m_holder.m_size))){
         //Expand forward
         this->priv_insert_forward_range_expand_forward
            (p, n, insert_range_proxy, dtl::bool_<dtl::is_single_value_proxy<InsertionProxy>::value>());
         return iterator(pos);
      }
      else{
         return this->priv_insert_forward_range_no_capacity(p, n, insert_range_proxy, alloc_version());
      }
   }

   template <class U>
   void priv_resize(const size_type new_size, const U &u, version_0)
   {
      const size_type sz = this->m_holder.m_size;
      if (new_size > this->capacity()){
         //This will trigger an error
         alloc_holder_t::on_capacity_overflow();
      }
      else if (new_size < sz){
         //Destroy last elements
         this->priv_destroy_last_n(sz - new_size);
      }
      else{
         T* const old_finish = this->priv_raw_end();
         this->priv_resize_proxy(u).uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, new_size - sz);
         this->m_holder.set_stored_size(new_size);
      }
   }

   template <class U, class AllocVersion>
   void priv_resize(const size_type new_size, const U &u, AllocVersion)
   {
      const size_type sz = this->m_holder.m_size;
      if (new_size < sz){
         //Destroy last elements
         this->priv_destroy_last_n(size_type(sz - new_size));
      }
      else {
         this->priv_insert_forward_range(this->back_ptr(), size_type(new_size - sz), this->priv_resize_proxy(u));
      }
   }

   //Takes the range pointed by [first_pos, last_pos) and shifts it to the right
   //by 'shift_count'. 'limit_pos' marks the end of constructed elements.
   //
   //Precondition: first_pos <= last_pos <= limit_pos
   //
   //The shift operation might cross limit_pos so elements to moved beyond limit_pos
   //are uninitialized_moved with an allocator. Other elements are moved.
   //
   //The shift operation might left uninitialized elements after limit_pos
   //and the number of uninitialized elements is returned by the function.
   //
   //Old situation:
   //       first_pos   last_pos         old_limit
   //             |       |                  |
   // ____________V_______V__________________V_____________
   //|   prefix   | range |     suffix       |raw_mem      ~
   //|____________|_______|__________________|_____________~
   //
   //New situation in Case A (hole_size == 0):
   // range is moved through move assignments
   //
   //       first_pos   last_pos         limit_pos
   //             |       |                  |
   // ____________V_______V__________________V_____________
   //|   prefix'  |       |  | range |suffix'|raw_mem      ~
   //|________________+______|___^___|_______|_____________~
   //                 |          |
   //                 |_>_>_>_>_>^
   //
   //
   //New situation in Case B (hole_size >= 0):
   // range is moved through uninitialized moves
   //
   //       first_pos   last_pos         limit_pos
   //             |       |                  |
   // ____________V_______V__________________V________________
   //|    prefix' |       |                  | [hole] | range |
   //|_______________________________________|________|___^___|
   //                 |                                   |
   //                 |_>_>_>_>_>_>_>_>_>_>_>_>_>_>_>_>_>_^
   //
   //New situation in Case C (hole_size == 0):
   // range is moved through move assignments and uninitialized moves
   //
   //       first_pos   last_pos         limit_pos
   //             |       |                  |
   // ____________V_______V__________________V___
   //|   prefix'  |       |              | range |
   //|___________________________________|___^___|
   //                 |                      |
   //                 |_>_>_>_>_>_>_>_>_>_>_>^
   size_type priv_insert_ordered_at_shift_range
      (size_type first_pos, size_type last_pos, size_type limit_pos, size_type shift_count)
   {
      BOOST_ASSERT(first_pos <= last_pos);
      BOOST_ASSERT(last_pos <= limit_pos);
      //
      T* const begin_ptr = this->priv_raw_begin();
      T* const first_ptr = begin_ptr + first_pos;
      T* const last_ptr  = begin_ptr + last_pos;

      size_type hole_size = 0;
      //Case A:
      if((last_pos + shift_count) <= limit_pos){
         //All move assigned
         boost::container::move_backward(first_ptr, last_ptr, last_ptr + shift_count);
      }
      //Case B:
      else if((first_pos + shift_count) >= limit_pos){
         //All uninitialized_moved
         ::boost::container::uninitialized_move_alloc
            (this->m_holder.alloc(), first_ptr, last_ptr, first_ptr + shift_count);
         //Cast in case size_type is narrower than int, promotions are applied
         //and Wconversion is in place
         hole_size = static_cast<size_type>(first_pos + shift_count - limit_pos);
      }
      //Case C:
      else{
         //Some uninitialized_moved
         T* const limit_ptr    = begin_ptr + limit_pos;
         T* const boundary_ptr = limit_ptr - shift_count;
         ::boost::container::uninitialized_move_alloc(this->m_holder.alloc(), boundary_ptr, last_ptr, limit_ptr);
         //The rest is move assigned
         boost::container::move_backward(first_ptr, boundary_ptr, limit_ptr);
      }
      return hole_size;
   }

   private:
   inline T *priv_raw_begin() const
   {  return boost::movelib::to_raw_pointer(m_holder.start());  }

   inline T* priv_raw_end() const
   {  return this->priv_raw_begin() + this->m_holder.m_size;  }

   template <class InsertionProxy>  //inline single-element version as it is significantly smaller
   inline void priv_insert_forward_range_expand_forward
      (T* const raw_pos, const size_type, InsertionProxy insert_range_proxy, dtl::true_type)
   {
      BOOST_ASSERT(this->room_enough());
      //There is enough memory
      T* const old_finish = this->priv_raw_end();
      allocator_type & a = this->m_holder.alloc();

      if (old_finish == raw_pos){
         insert_range_proxy.uninitialized_copy_n_and_update(a, old_finish, 1);
         ++this->m_holder.m_size;
      }
      else{
         //New elements can be just copied.
         //Move to uninitialized memory last objects
         T * const before_old_finish = old_finish-1;

         allocator_traits_type::construct(a, old_finish, ::boost::move(*before_old_finish));
         ++this->m_holder.m_size;
         //Copy previous to last objects to the initialized end
         boost::container::move_backward(raw_pos, before_old_finish, old_finish);
         //Insert new objects in the raw_pos
         insert_range_proxy.copy_n_and_update(a, raw_pos, 1);
      }
   }

   template <class InsertionProxy>
   inline void priv_insert_forward_range_expand_forward
      (T* const raw_pos, const size_type n, InsertionProxy insert_range_proxy, dtl::false_type)
   {
      //There is enough memory
      boost::container::expand_forward_and_insert_alloc
         ( this->m_holder.alloc(), raw_pos, this->priv_raw_end(), n, insert_range_proxy);
      this->m_holder.inc_stored_size(n);
   }

   template <class InsertionProxy>
   void priv_insert_forward_range_new_allocation
      (T* const new_start, size_type new_cap, T* const pos, const size_type n, InsertionProxy insert_range_proxy)
   {
      //n can be zero, if we want to reallocate!
      allocator_type &a =  this->m_holder.alloc();
      T * const raw_old_buffer = this->priv_raw_begin();

      typename value_traits::ArrayDeallocator new_buffer_deallocator(new_start, a, new_cap);
      boost::container::uninitialized_move_and_insert_alloc
         (a, raw_old_buffer, pos, this->priv_raw_end(), new_start, n, insert_range_proxy);
      new_buffer_deallocator.release();

      //Destroy and deallocate old elements
      if(raw_old_buffer){
         BOOST_IF_CONSTEXPR(!has_trivial_destructor_after_move<value_type>::value)
            boost::container::destroy_alloc_n(a, raw_old_buffer, this->m_holder.m_size);
         this->m_holder.deallocate(this->m_holder.start(), this->m_holder.capacity());
      }

      this->m_holder.start(new_start);
      this->m_holder.inc_stored_size(n);
      this->m_holder.capacity(new_cap);
   }

   template <class InsertionProxy>
   void priv_insert_forward_range_expand_backwards
         (T* const new_start, const size_type new_capacity,
          T* const pos, const size_type n, InsertionProxy insert_range_proxy)
   {
      T* const old_start = this->priv_raw_begin();
      const size_type old_size = this->m_holder.m_size;
      allocator_type& a = this->m_holder.alloc();

      //Update the vector buffer information to a safe state
      this->m_holder.start(new_start);
      this->m_holder.capacity(new_capacity);
      this->m_holder.m_size = 0;

      expand_backward_forward_and_insert_alloc(old_start, old_size, new_start, pos, n, insert_range_proxy, a);

      //Update the vector buffer information to a safe state
      this->m_holder.m_size = stored_size_type(old_size + n);
   }

   void priv_throw_if_out_of_range(size_type n) const
   {
      //If n is out of range, throw an out_of_range exception
      if (n >= this->size()){
         throw_out_of_range("vector::at out of range");
      }
   }

   inline bool priv_in_range(const_iterator pos) const
   {
      return (this->begin() <= pos) && (pos < this->end());
   }

   inline bool priv_in_range_or_end(const_iterator pos) const
   {
      return (this->begin() <= pos) && (pos <= this->end());
   }

   #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
   public:
   unsigned int num_expand_fwd;
   unsigned int num_expand_bwd;
   unsigned int num_shrink;
   unsigned int num_alloc;
   void reset_alloc_stats()
   {  num_expand_fwd = num_expand_bwd = num_alloc = 0, num_shrink = 0;   }
   #endif
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

#ifndef BOOST_CONTAINER_NO_CXX17_CTAD

template <typename InputIterator>
vector(InputIterator, InputIterator) ->
   vector<typename iter_value<InputIterator>::type>;

template <typename InputIterator, typename Allocator>
vector(InputIterator, InputIterator, Allocator const&) ->
   vector<typename iter_value<InputIterator>::type, Allocator>;

#endif


}} //namespace boost::container

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

namespace boost {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator, class Options>
struct has_trivial_destructor_after_move<boost::container::vector<T, Allocator, Options> >
{
   typedef typename boost::container::vector<T, Allocator, Options>::allocator_type allocator_type;
   typedef typename ::boost::container::allocator_traits<allocator_type>::pointer pointer;
   BOOST_STATIC_CONSTEXPR bool value = ::boost::has_trivial_destructor_after_move<allocator_type>::value &&
                                                ::boost::has_trivial_destructor_after_move<pointer>::value;
};

}

//See comments on vec_iterator::element_type to know why is this needed
#ifdef BOOST_GNU_STDLIB

#include <boost/move/detail/std_ns_begin.hpp>

BOOST_MOVE_STD_NS_BEG

template <class Pointer, bool IsConst>
struct pointer_traits< boost::container::vec_iterator<Pointer, IsConst> >
   : public boost::intrusive::pointer_traits< boost::container::vec_iterator<Pointer, IsConst> >
{};

BOOST_MOVE_STD_NS_END

#include <boost/move/detail/std_ns_end.hpp>

#endif   //BOOST_GNU_STDLIB

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

#include <boost/container/detail/config_end.hpp>

#endif //   #ifndef  BOOST_CONTAINER_CONTAINER_VECTOR_HPP
