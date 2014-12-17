//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_VECTOR_HPP
#define BOOST_CONTAINER_CONTAINER_VECTOR_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/container_fwd.hpp>

#include <cstddef>
#include <memory>
#include <algorithm>
#include <iterator>
#include <utility>
#include <boost/detail/no_exceptions_support.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/type_traits/has_trivial_copy.hpp>
#include <boost/type_traits/has_trivial_assign.hpp>
#include <boost/type_traits/has_nothrow_copy.hpp>
#include <boost/type_traits/has_nothrow_assign.hpp>
#include <boost/type_traits/has_nothrow_constructor.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/allocation_type.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/algorithms.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/detail/allocator_version_traits.hpp>
#include <boost/container/throw_exception.hpp>
#include <boost/move/utility.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/detail/move_helpers.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/advanced_insert_int.hpp>
#include <boost/assert.hpp>

namespace boost {
namespace container {

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//#define BOOST_CONTAINER_VECTOR_ITERATOR_IS_POINTER

namespace container_detail {

#ifndef BOOST_CONTAINER_VECTOR_ITERATOR_IS_POINTER

template <class Pointer, bool IsConst>
class vec_iterator
{
   public:
   typedef std::random_access_iterator_tag                                          iterator_category;
   typedef typename boost::intrusive::pointer_traits<Pointer>::element_type         value_type;
   typedef typename boost::intrusive::pointer_traits<Pointer>::difference_type      difference_type;
   typedef typename if_c
      < IsConst
      , typename boost::intrusive::pointer_traits<Pointer>::template
                                 rebind_pointer<const value_type>::type
      , Pointer
      >::type                                                                       pointer;
   typedef typename boost::intrusive::pointer_traits<Pointer>                       ptr_traits;
   typedef typename ptr_traits::reference                                           reference;

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   Pointer m_ptr;

   public:
   const Pointer &get_ptr() const BOOST_CONTAINER_NOEXCEPT
   {  return   m_ptr;  }

   Pointer &get_ptr() BOOST_CONTAINER_NOEXCEPT
   {  return   m_ptr;  }

   explicit vec_iterator(Pointer ptr) BOOST_CONTAINER_NOEXCEPT
      : m_ptr(ptr)
   {}
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:

   //Constructors
   vec_iterator() BOOST_CONTAINER_NOEXCEPT
      #ifndef NDEBUG
      : m_ptr()
      #else
      // No value initialization of m_ptr() to speed up things a bit:
      #endif
   {}

   vec_iterator(vec_iterator<Pointer, false> const& other) BOOST_CONTAINER_NOEXCEPT
      :  m_ptr(other.get_ptr())
   {}

   //Pointer like operators
   reference operator*()   const BOOST_CONTAINER_NOEXCEPT
   {  return *m_ptr;  }

   pointer operator->()  const BOOST_CONTAINER_NOEXCEPT
   {  return ::boost::intrusive::pointer_traits<pointer>::pointer_to(this->operator*());  }

   reference operator[](difference_type off) const BOOST_CONTAINER_NOEXCEPT
   {  return m_ptr[off];   }

   //Increment / Decrement
   vec_iterator& operator++() BOOST_CONTAINER_NOEXCEPT
   { ++m_ptr;  return *this; }

   vec_iterator operator++(int) BOOST_CONTAINER_NOEXCEPT
   {  return vec_iterator(m_ptr++); }

   vec_iterator& operator--() BOOST_CONTAINER_NOEXCEPT
   {  --m_ptr; return *this;  }

   vec_iterator operator--(int) BOOST_CONTAINER_NOEXCEPT
   {  return vec_iterator(m_ptr--); }

   //Arithmetic
   vec_iterator& operator+=(difference_type off) BOOST_CONTAINER_NOEXCEPT
   {  m_ptr += off; return *this;   }

   vec_iterator& operator-=(difference_type off) BOOST_CONTAINER_NOEXCEPT
   {  m_ptr -= off; return *this;   }

   friend vec_iterator operator+(const vec_iterator &x, difference_type off) BOOST_CONTAINER_NOEXCEPT
   {  return vec_iterator(x.m_ptr+off);  }

   friend vec_iterator operator+(difference_type off, vec_iterator right) BOOST_CONTAINER_NOEXCEPT
   {  right.m_ptr += off;  return right; }

   friend vec_iterator operator-(vec_iterator left, difference_type off) BOOST_CONTAINER_NOEXCEPT
   {  left.m_ptr -= off;  return left; }

   friend difference_type operator-(const vec_iterator &left, const vec_iterator& right) BOOST_CONTAINER_NOEXCEPT
   {  return left.m_ptr - right.m_ptr;   }

   //Comparison operators
   friend bool operator==   (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr == r.m_ptr;  }

   friend bool operator!=   (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr != r.m_ptr;  }

   friend bool operator<    (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr < r.m_ptr;  }

   friend bool operator<=   (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr <= r.m_ptr;  }

   friend bool operator>    (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr > r.m_ptr;  }

   friend bool operator>=   (const vec_iterator& l, const vec_iterator& r) BOOST_CONTAINER_NOEXCEPT
   {  return l.m_ptr >= r.m_ptr;  }
};

}  //namespace container_detail {

template<class Pointer, bool IsConst>
const Pointer &vector_iterator_get_ptr(const container_detail::vec_iterator<Pointer, IsConst> &it) BOOST_CONTAINER_NOEXCEPT
{  return   it.get_ptr();  }

template<class Pointer, bool IsConst>
Pointer &get_ptr(container_detail::vec_iterator<Pointer, IsConst> &it) BOOST_CONTAINER_NOEXCEPT
{  return  it.get_ptr();  }

namespace container_detail {

#else //ifndef BOOST_CONTAINER_VECTOR_ITERATOR_IS_POINTER

template< class MaybeConstPointer
        , bool ElementTypeIsConst
            = is_const< typename boost::intrusive::pointer_traits<MaybeConstPointer>::element_type>::value >
struct vector_get_ptr_pointer_to_non_const
{
   typedef MaybeConstPointer                                         const_pointer;
   typedef boost::intrusive::pointer_traits<const_pointer>           pointer_traits_t;
   typedef typename pointer_traits_t::element_type                   element_type;
   typedef typename remove_const<element_type>::type                 non_const_element_type;
   typedef typename pointer_traits_t
      ::template rebind_pointer<non_const_element_type>::type        return_type;

   static return_type get_ptr(const const_pointer &ptr) BOOST_CONTAINER_NOEXCEPT
   {  return boost::intrusive::pointer_traits<return_type>::const_cast_from(ptr);  }
};

template<class Pointer>
struct vector_get_ptr_pointer_to_non_const<Pointer, false>
{
   typedef const Pointer & return_type;
   static return_type get_ptr(const Pointer &ptr) BOOST_CONTAINER_NOEXCEPT
   {  return ptr;  }
};

}  //namespace container_detail {

template<class MaybeConstPointer>
typename container_detail::vector_get_ptr_pointer_to_non_const<MaybeConstPointer>::return_type
   vector_iterator_get_ptr(const MaybeConstPointer &ptr) BOOST_CONTAINER_NOEXCEPT
{
   return container_detail::vector_get_ptr_pointer_to_non_const<MaybeConstPointer>::get_ptr(ptr);
}

namespace container_detail {

#endif   //#ifndef BOOST_CONTAINER_VECTOR_ITERATOR_IS_POINTER

struct uninitialized_size_t {};
static const uninitialized_size_t uninitialized_size = uninitialized_size_t();

template <class T, class Allocator>
struct vector_value_traits
{
   typedef T value_type;
   typedef Allocator allocator_type;
   static const bool trivial_dctr = boost::has_trivial_destructor<value_type>::value;
   static const bool trivial_dctr_after_move = ::boost::has_trivial_destructor_after_move<value_type>::value;
   static const bool trivial_copy = has_trivial_copy<value_type>::value;
   static const bool nothrow_copy = has_nothrow_copy<value_type>::value || trivial_copy;
   static const bool trivial_assign = has_trivial_assign<value_type>::value;
   static const bool nothrow_assign = has_nothrow_assign<value_type>::value || trivial_assign;

   //This is the anti-exception array destructor
   //to deallocate values already constructed
   typedef typename container_detail::if_c
      <trivial_dctr
      ,container_detail::null_scoped_destructor_n<Allocator>
      ,container_detail::scoped_destructor_n<Allocator>
      >::type   ArrayDestructor;
   //This is the anti-exception array deallocator
   typedef typename container_detail::if_c
      <false//nothrow_copy
      ,container_detail::null_scoped_array_deallocator<Allocator>
      ,container_detail::scoped_array_deallocator<Allocator>
      >::type   ArrayDeallocator;
};

//!This struct deallocates and allocated memory
template < class Allocator
         , class AllocatorVersion = typename container_detail::version<Allocator>::type
         >
struct vector_alloc_holder
   : public Allocator
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(vector_alloc_holder)

   public:
   typedef boost::container::allocator_traits<Allocator> allocator_traits_type;
   typedef typename allocator_traits_type::pointer       pointer;
   typedef typename allocator_traits_type::size_type     size_type;
   typedef typename allocator_traits_type::value_type    value_type;

   //Constructor, does not throw
   vector_alloc_holder()
      BOOST_CONTAINER_NOEXCEPT_IF(::boost::has_nothrow_default_constructor<Allocator>::value)
      : Allocator(), m_start(), m_size(), m_capacity()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   explicit vector_alloc_holder(BOOST_FWD_REF(AllocConvertible) a) BOOST_CONTAINER_NOEXCEPT
      : Allocator(boost::forward<AllocConvertible>(a)), m_start(), m_size(), m_capacity()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   vector_alloc_holder(uninitialized_size_t, BOOST_FWD_REF(AllocConvertible) a, size_type initial_size)
      : Allocator(boost::forward<AllocConvertible>(a))
      , m_start()
      , m_size(initial_size)  //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_capacity()
   {
      if(initial_size){
         m_start = this->allocation_command(allocate_new, initial_size, initial_size, m_capacity, m_start).first;
      }
   }

   //Constructor, does not throw
   vector_alloc_holder(uninitialized_size_t, size_type initial_size)
      : Allocator()
      , m_start()
      , m_size(initial_size)  //Size is initialized here so vector should only call uninitialized_xxx after this
      , m_capacity()
   {
      if(initial_size){
         m_start = this->allocation_command
               (allocate_new, initial_size, initial_size, m_capacity, m_start).first;
      }
   }

   vector_alloc_holder(BOOST_RV_REF(vector_alloc_holder) holder) BOOST_CONTAINER_NOEXCEPT
      : Allocator(boost::move(static_cast<Allocator&>(holder)))
      , m_start(holder.m_start)
      , m_size(holder.m_size)
      , m_capacity(holder.m_capacity)
   {
      holder.m_start = pointer();
      holder.m_size = holder.m_capacity = 0;
   }

   void first_allocation(size_type cap)
   {
      if(cap){
         m_start = this->allocation_command
               (allocate_new, cap, cap, m_capacity, m_start).first;
      }
   }

   void first_allocation_same_allocator_type(size_type cap)
   {  this->first_allocation(cap);  }

   ~vector_alloc_holder() BOOST_CONTAINER_NOEXCEPT
   {
      if(this->m_capacity){
         this->alloc().deallocate(this->m_start, this->m_capacity);
      }
   }

   std::pair<pointer, bool>
      allocation_command(boost::container::allocation_type command,
                         size_type limit_size,
                         size_type preferred_size,
                         size_type &received_size, const pointer &reuse = pointer())
   {
      return allocator_version_traits<Allocator>::allocation_command
         (this->alloc(), command, limit_size, preferred_size, received_size, reuse);
   }

   size_type next_capacity(size_type additional_objects) const
   {
      return next_capacity_calculator
         <size_type, NextCapacityDouble/*NextCapacity60Percent*/>::
            get( allocator_traits_type::max_size(this->alloc())
               , this->m_capacity, additional_objects );
   }

   pointer     m_start;
   size_type   m_size;
   size_type   m_capacity;

   void swap(vector_alloc_holder &x) BOOST_CONTAINER_NOEXCEPT
   {
      boost::container::swap_dispatch(this->m_start, x.m_start);
      boost::container::swap_dispatch(this->m_size, x.m_size);
      boost::container::swap_dispatch(this->m_capacity, x.m_capacity);
   }

   void move_from_empty(vector_alloc_holder &x) BOOST_CONTAINER_NOEXCEPT
   {
      //this->m_size was previously initialized
      this->m_start     = x.m_start;
      this->m_capacity  = x.m_capacity;
      x.m_start = pointer();
      x.m_size = x.m_capacity = 0;
   }

   Allocator &alloc() BOOST_CONTAINER_NOEXCEPT
   {  return *this;  }

   const Allocator &alloc() const BOOST_CONTAINER_NOEXCEPT
   {  return *this;  }

   const pointer   &start() const     BOOST_CONTAINER_NOEXCEPT {  return m_start;  }
   const size_type &capacity() const  BOOST_CONTAINER_NOEXCEPT {  return m_capacity;  }
   void start(const pointer &p)       BOOST_CONTAINER_NOEXCEPT {  m_start = p;  }
   void capacity(const size_type &c)  BOOST_CONTAINER_NOEXCEPT {  m_capacity = c;  }
};

//!This struct deallocates and allocated memory
template <class Allocator>
struct vector_alloc_holder<Allocator, container_detail::integral_constant<unsigned, 0> >
   : public Allocator
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(vector_alloc_holder)

   public:
   typedef boost::container::allocator_traits<Allocator> allocator_traits_type;
   typedef typename allocator_traits_type::pointer       pointer;
   typedef typename allocator_traits_type::size_type     size_type;
   typedef typename allocator_traits_type::value_type    value_type;

   template <class OtherAllocator, class OtherAllocatorVersion>
   friend struct vector_alloc_holder;

   //Constructor, does not throw
   vector_alloc_holder()
      BOOST_CONTAINER_NOEXCEPT_IF(::boost::has_nothrow_default_constructor<Allocator>::value)
      : Allocator(), m_size()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   explicit vector_alloc_holder(BOOST_FWD_REF(AllocConvertible) a) BOOST_CONTAINER_NOEXCEPT
      : Allocator(boost::forward<AllocConvertible>(a)), m_size()
   {}

   //Constructor, does not throw
   template<class AllocConvertible>
   vector_alloc_holder(uninitialized_size_t, BOOST_FWD_REF(AllocConvertible) a, size_type initial_size)
      : Allocator(boost::forward<AllocConvertible>(a))
      , m_size(initial_size)  //Size is initialized here...
   {
      //... and capacity here, so vector, must call uninitialized_xxx in the derived constructor
      this->first_allocation(initial_size);
   }

   //Constructor, does not throw
   vector_alloc_holder(uninitialized_size_t, size_type initial_size)
      : Allocator()
      , m_size(initial_size)  //Size is initialized here...
   {
      //... and capacity here, so vector, must call uninitialized_xxx in the derived constructor
      this->first_allocation(initial_size);
   }

   vector_alloc_holder(BOOST_RV_REF(vector_alloc_holder) holder)
      : Allocator(boost::move(static_cast<Allocator&>(holder)))
      , m_size(holder.m_size) //Size is initialized here so vector should only call uninitialized_xxx after this
   {
      ::boost::container::uninitialized_move_alloc_n
         (this->alloc(), container_detail::to_raw_pointer(holder.start()), m_size, container_detail::to_raw_pointer(this->start()));
   }

   template<class OtherAllocator, class OtherAllocatorVersion>
   vector_alloc_holder(BOOST_RV_REF_BEG vector_alloc_holder<OtherAllocator, OtherAllocatorVersion> BOOST_RV_REF_END holder)
      : Allocator()
      , m_size(holder.m_size) //Initialize it to m_size as first_allocation can only succeed or abort
   {
      //Different allocator type so we must check we have enough storage
      const size_type n = holder.m_size;
      this->first_allocation(n);
      ::boost::container::uninitialized_move_alloc_n
         (this->alloc(), container_detail::to_raw_pointer(holder.start()), n, container_detail::to_raw_pointer(this->start()));
   }

   void first_allocation(size_type cap)
   {
      if(cap > Allocator::internal_capacity){
         throw_bad_alloc();
      }
   }

   void first_allocation_same_allocator_type(size_type) BOOST_CONTAINER_NOEXCEPT
   {}

   //Destructor
   ~vector_alloc_holder() BOOST_CONTAINER_NOEXCEPT
   {}

   void swap(vector_alloc_holder &x)
   {
      this->priv_swap_members_impl(x);
   }

   template<class OtherAllocator, class OtherAllocatorVersion>
   void swap(vector_alloc_holder<OtherAllocator, OtherAllocatorVersion> &x)
   {
      if(this->m_size > OtherAllocator::internal_capacity || x.m_size > Allocator::internal_capacity){
         throw_bad_alloc();
      }
      this->priv_swap_members_impl(x);
   }

   void move_from_empty(vector_alloc_holder &)
   {  //Containers with version 0 allocators can't be moved without move elements one by one
      throw_bad_alloc();
   }

   Allocator &alloc() BOOST_CONTAINER_NOEXCEPT
   {  return *this;  }

   const Allocator &alloc() const BOOST_CONTAINER_NOEXCEPT
   {  return *this;  }

   pointer start() const       BOOST_CONTAINER_NOEXCEPT {  return Allocator::internal_storage();  }
   size_type  capacity() const BOOST_CONTAINER_NOEXCEPT {  return Allocator::internal_capacity;  }
   size_type   m_size;

   private:

   template<class OtherAllocator, class OtherAllocatorVersion>
   void priv_swap_members_impl(vector_alloc_holder<OtherAllocator, OtherAllocatorVersion> &x)
   {
      const std::size_t MaxTmpStorage = sizeof(value_type)*Allocator::internal_capacity;
      value_type *const first_this = container_detail::to_raw_pointer(this->start());
      value_type *const first_x = container_detail::to_raw_pointer(x.start());

      if(this->m_size < x.m_size){
         boost::container::deep_swap_alloc_n<MaxTmpStorage>(this->alloc(), first_this, this->m_size, first_x, x.m_size);
      }
      else{
         boost::container::deep_swap_alloc_n<MaxTmpStorage>(this->alloc(), first_x, x.m_size, first_this, this->m_size);
      }
      boost::container::swap_dispatch(this->m_size, x.m_size);
   }
};

}  //namespace container_detail {

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//! A vector is a sequence that supports random access to elements, constant
//! time insertion and removal of elements at the end, and linear time insertion
//! and removal of elements at the beginning or in the middle. The number of
//! elements in a vector may vary dynamically; memory management is automatic.
//!
//! \tparam T The type of object that is stored in the vector
//! \tparam Allocator The allocator used for all internal memory management
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Allocator = std::allocator<T> >
#else
template <class T, class Allocator>
#endif
class vector
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   typedef typename container_detail::version<Allocator>::type alloc_version;
   boost::container::container_detail::vector_alloc_holder
      <Allocator, alloc_version>                            m_holder;
   typedef allocator_traits<Allocator>                      allocator_traits_type;
   template <class U, class UAllocator>
   friend class vector;

   typedef typename ::boost::container::allocator_traits
      <Allocator>::pointer                                     pointer_impl;
   typedef container_detail::vec_iterator<pointer_impl, false> iterator_impl;
   typedef container_detail::vec_iterator<pointer_impl, true > const_iterator_impl;

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   public:
   //////////////////////////////////////////////
   //
   //                    types
   //
   //////////////////////////////////////////////

   typedef T                                                                           value_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer           pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_pointer     const_pointer;
   typedef typename ::boost::container::allocator_traits<Allocator>::reference         reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::const_reference   const_reference;
   typedef typename ::boost::container::allocator_traits<Allocator>::size_type         size_type;
   typedef typename ::boost::container::allocator_traits<Allocator>::difference_type   difference_type;
   typedef Allocator                                                                   allocator_type;
   typedef Allocator                                                                   stored_allocator_type;
   #if defined BOOST_CONTAINER_VECTOR_ITERATOR_IS_POINTER && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   typedef BOOST_CONTAINER_IMPDEF(pointer)                                             iterator;
   typedef BOOST_CONTAINER_IMPDEF(const_pointer)                                       const_iterator;
   #else
   typedef BOOST_CONTAINER_IMPDEF(iterator_impl)                                       iterator;
   typedef BOOST_CONTAINER_IMPDEF(const_iterator_impl)                                 const_iterator;
   #endif
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<iterator>)                     reverse_iterator;
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<const_iterator>)               const_reverse_iterator;

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:
   BOOST_COPYABLE_AND_MOVABLE(vector)
   typedef container_detail::vector_value_traits<value_type, Allocator> value_traits;

   typedef container_detail::integral_constant<unsigned, 0> allocator_v0;
   typedef container_detail::integral_constant<unsigned, 1> allocator_v1;
   typedef container_detail::integral_constant<unsigned, 2> allocator_v2;

   typedef constant_iterator<T, difference_type>            cvalue_iterator;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Constructs a vector taking the allocator as parameter.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   vector()
      BOOST_CONTAINER_NOEXCEPT_IF(::boost::has_nothrow_default_constructor<Allocator>::value)
      : m_holder()
   {}

   //! <b>Effects</b>: Constructs a vector taking the allocator as parameter.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   explicit vector(const Allocator& a) BOOST_CONTAINER_NOEXCEPT
      : m_holder(a)
   {}

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n value initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's value initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   explicit vector(size_type n)
      :  m_holder(container_detail::uninitialized_size, n)
   {
      boost::container::uninitialized_value_init_alloc_n
         (this->m_holder.alloc(), n, container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n default initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's default initialization throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   //!
   //! <b>Note</b>: Non-standard extension
   vector(size_type n, default_init_t)
      :  m_holder(container_detail::uninitialized_size, n)
   {
      boost::container::uninitialized_default_init_alloc_n
         (this->m_holder.alloc(), n, container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Constructs a vector
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   vector(size_type n, const T& value)
      :  m_holder(container_detail::uninitialized_size, n)
   {
      boost::container::uninitialized_fill_alloc_n
         (this->m_holder.alloc(), value, n, container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   vector(size_type n, const T& value, const allocator_type& a)
      :  m_holder(container_detail::uninitialized_size, a, n)
   {
      boost::container::uninitialized_fill_alloc_n
         (this->m_holder.alloc(), value, n, container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Constructs a vector
   //!   and inserts a copy of the range [first, last) in the vector.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's constructor taking a dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InIt>
   vector(InIt first, InIt last)
      :  m_holder()
   {  this->insert(this->cend(), first, last); }

   //! <b>Effects</b>: Constructs a vector that will use a copy of allocator a
   //!   and inserts a copy of the range [first, last) in the vector.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's constructor taking a dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InIt>
   vector(InIt first, InIt last, const allocator_type& a)
      :  m_holder(a)
   {  this->insert(this->cend(), first, last); }

   //! <b>Effects</b>: Copy constructs a vector.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   vector(const vector &x)
      :  m_holder( container_detail::uninitialized_size
                 , allocator_traits_type::select_on_container_copy_construction(x.m_holder.alloc())
                 , x.size())
   {
      ::boost::container::uninitialized_copy_alloc_n
         ( this->m_holder.alloc(), container_detail::to_raw_pointer(x.m_holder.start())
         , x.size(), container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Move constructor. Moves x's resources to *this.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   vector(BOOST_RV_REF(vector) x) BOOST_CONTAINER_NOEXCEPT
      :  m_holder(boost::move(x.m_holder))
   {}

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Move constructor. Moves x's resources to *this.
   //!
   //! <b>Throws</b>: If T's move constructor or allocation throws
   //!
   //! <b>Complexity</b>: Linear.
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherAllocator>
   vector(BOOST_RV_REF_BEG vector<T, OtherAllocator> BOOST_RV_REF_END x
         , typename container_detail::enable_if_c
            < container_detail::is_version<OtherAllocator, 0>::value>::type * = 0
         )
      :  m_holder(boost::move(x.m_holder))
   {}

   #endif   //!defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Copy constructs a vector using the specified allocator.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   vector(const vector &x, const allocator_type &a)
      :  m_holder(container_detail::uninitialized_size, a, x.size())
   {
      ::boost::container::uninitialized_copy_alloc_n_source
         ( this->m_holder.alloc(), container_detail::to_raw_pointer(x.m_holder.start())
         , x.size(), container_detail::to_raw_pointer(this->m_holder.start()));
   }

   //! <b>Effects</b>: Move constructor using the specified allocator.
   //!                 Moves x's resources to *this if a == allocator_type().
   //!                 Otherwise copies values from x to *this.
   //!
   //! <b>Throws</b>: If allocation or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant if a == x.get_allocator(), linear otherwise.
   vector(BOOST_RV_REF(vector) x, const allocator_type &a)
      :  m_holder(container_detail::uninitialized_size, a, x.size())
   {
      if(x.m_holder.alloc() == a){
         this->m_holder.move_from_empty(x.m_holder);
      }
      else{
         const size_type n = x.size();
         this->m_holder.first_allocation_same_allocator_type(n);
         ::boost::container::uninitialized_move_alloc_n_source
            ( this->m_holder.alloc(), container_detail::to_raw_pointer(x.m_holder.start())
            , n, container_detail::to_raw_pointer(this->m_holder.start()));
      }
   }

   //! <b>Effects</b>: Destroys the vector. All stored values are destroyed
   //!   and used memory is deallocated.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements.
   ~vector() BOOST_CONTAINER_NOEXCEPT
   {
      boost::container::destroy_alloc_n
         (this->get_stored_allocator(), container_detail::to_raw_pointer(this->m_holder.start()), this->m_holder.m_size);
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
   vector& operator=(BOOST_COPY_ASSIGN_REF(vector) x)
   {
      if (&x != this){
         this->priv_copy_assign(x);
      }
      return *this;
   }

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
   vector& operator=(BOOST_RV_REF(vector) x)
      BOOST_CONTAINER_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value)
   {
      this->priv_move_assign(boost::move(x));
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
   template<class OtherAllocator>
   typename container_detail::enable_if_c
                           < container_detail::is_version<OtherAllocator, 0>::value &&
                            !container_detail::is_same<OtherAllocator, allocator_type>::value
                           , vector& >::type
      operator=(BOOST_RV_REF_BEG vector<value_type, OtherAllocator> BOOST_RV_REF_END x)
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
   template<class OtherAllocator>
   typename container_detail::enable_if_c
                           < container_detail::is_version<OtherAllocator, 0>::value &&
                            !container_detail::is_same<OtherAllocator, allocator_type>::value
                           , vector& >::type
      operator=(const vector<value_type, OtherAllocator> &x)
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
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InIt, size_type>::value &&
            ( container_detail::is_input_iterator<InIt>::value ||
              container_detail::is_same<alloc_version, allocator_v0>::value )
         >::type * = 0
      #endif
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
         T* const end_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
         size_type n = static_cast<size_type>(end_pos - container_detail::to_raw_pointer(vector_iterator_get_ptr(cur)));
         this->priv_destroy_last_n(n);
      }
      else{
         //There are more elements in the range, insert the remaining ones
         this->insert(this->cend(), first, last);
      }
   }

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy/move constructor/assignment or
   //!   T's constructor/assignment from dereferencing InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template <class FwdIt>
   void assign(FwdIt first, FwdIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<FwdIt, size_type>::value &&
            ( !container_detail::is_input_iterator<FwdIt>::value &&
              !container_detail::is_same<alloc_version, allocator_v0>::value )
         >::type * = 0
      #endif
      )
   {
      //For Fwd iterators the standard only requires EmplaceConstructible and assignable from *first
      //so we can't do any backwards allocation
      const size_type input_sz = static_cast<size_type>(std::distance(first, last));
      const size_type old_capacity = this->capacity();
      if(input_sz > old_capacity){  //If input range is too big, we need to reallocate
         size_type real_cap = 0;
         std::pair<pointer, bool> ret =
            this->m_holder.allocation_command(allocate_new, input_sz, input_sz, real_cap, this->m_holder.start());
         if(!ret.second){  //New allocation, just emplace new values
            pointer const old_p = this->m_holder.start();
            if(old_p){
               this->priv_destroy_all();
               this->m_holder.alloc().deallocate(old_p, old_capacity);
            }
            this->m_holder.start(ret.first);
            this->m_holder.capacity(real_cap);
            this->m_holder.m_size = 0;
            this->priv_uninitialized_construct_at_end(first, last);
            return;
         }
         else{
            //Forward expansion, use assignment + back deletion/construction that comes later
         }
      }
      //Overwrite all elements we can from [first, last)
      iterator cur = this->begin();
      const iterator end_it = this->end();
      for ( ; first != last && cur != end_it; ++cur, ++first){
         *cur = *first;
      }

      if (first == last){
         //There are no more elements in the sequence, erase remaining
         this->priv_destroy_last_n(this->size() - input_sz);
      }
      else{
         //Uninitialized construct at end the remaining range
         this->priv_uninitialized_construct_at_end(first, last);
      }
   }

   //! <b>Effects</b>: Assigns the n copies of val to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   void assign(size_type n, const value_type& val)
   {  this->assign(cvalue_iterator(val, n), cvalue_iterator());   }

   //! <b>Effects</b>: Returns a copy of the internal allocator.
   //!
   //! <b>Throws</b>: If allocator's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.alloc();  }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT
   {  return this->m_holder.alloc(); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT
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
   iterator begin() BOOST_CONTAINER_NOEXCEPT
   { return iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const BOOST_CONTAINER_NOEXCEPT
   { return const_iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns an iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator end() BOOST_CONTAINER_NOEXCEPT
   { return iterator(this->m_holder.start() + this->m_holder.m_size); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT
   { return this->cend(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT
   { return reverse_iterator(this->end());      }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT
   { return this->crbegin(); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT
   { return reverse_iterator(this->begin());       }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT
   { return this->crend(); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const BOOST_CONTAINER_NOEXCEPT
   { return const_iterator(this->m_holder.start()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT
   { return const_iterator(this->m_holder.start() + this->m_holder.m_size); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT
   { return const_reverse_iterator(this->end());}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT
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
   bool empty() const BOOST_CONTAINER_NOEXCEPT
   { return !this->m_holder.m_size; }

   //! <b>Effects</b>: Returns the number of the elements contained in the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type size() const BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.m_size; }

   //! <b>Effects</b>: Returns the largest possible size of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type max_size() const BOOST_CONTAINER_NOEXCEPT
   { return allocator_traits_type::max_size(this->m_holder.alloc()); }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are value initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move or value initialization throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size)
   {  this->priv_resize(new_size, value_init);  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are default initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move or default initialization throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   //!
   //! <b>Note</b>: Non-standard extension
   void resize(size_type new_size, default_init_t)
   {  this->priv_resize(new_size, default_init);  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are copy constructed from x.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size, const T& x)
   {  this->priv_resize(new_size, x);  }

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type capacity() const BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.capacity(); }

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //!
   //! <b>Throws</b>: If memory allocation allocation throws or T's copy/move constructor throws.
   void reserve(size_type new_cap)
   {
      if (this->capacity() < new_cap){
         this->priv_reserve(new_cap, alloc_version());
      }
   }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //!   with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit()
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
   reference         front() BOOST_CONTAINER_NOEXCEPT
   { return *this->m_holder.start(); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference   front() const BOOST_CONTAINER_NOEXCEPT
   { return *this->m_holder.start(); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference         back() BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.start()[this->m_holder.m_size - 1]; }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference   back()  const BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.start()[this->m_holder.m_size - 1]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference operator[](size_type n) BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.start()[n]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference operator[](size_type n) const BOOST_CONTAINER_NOEXCEPT
   { return this->m_holder.start()[n]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: std::range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   reference at(size_type n)
   { this->priv_check_range(n); return this->m_holder.start()[n]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: std::range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   const_reference at(size_type n) const
   { this->priv_check_range(n); return this->m_holder.start()[n]; }

   //////////////////////////////////////////////
   //
   //                 data access
   //
   //////////////////////////////////////////////

   //! <b>Returns</b>: Allocator pointer such that [data(),data() + size()) is a valid range.
   //!   For a non-empty vector, data() == &front().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   T* data() BOOST_CONTAINER_NOEXCEPT
   { return container_detail::to_raw_pointer(this->m_holder.start()); }

   //! <b>Returns</b>: Allocator pointer such that [data(),data() + size()) is a valid range.
   //!   For a non-empty vector, data() == &front().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const T * data()  const BOOST_CONTAINER_NOEXCEPT
   { return container_detail::to_raw_pointer(this->m_holder.start()); }

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the vector.
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws or
   //!   T's copy/move constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   template<class ...Args>
   void emplace_back(Args &&...args)
   {
      T* back_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
      if (this->m_holder.m_size < this->m_holder.capacity()){
         //There is more memory, just construct a new object at the end
         allocator_traits_type::construct(this->m_holder.alloc(), back_pos, ::boost::forward<Args>(args)...);
         ++this->m_holder.m_size;
      }
      else{
         typedef container_detail::insert_emplace_proxy<Allocator, T*, Args...> type;
         this->priv_forward_range_insert_no_capacity
            (vector_iterator_get_ptr(this->cend()), 1, type(::boost::forward<Args>(args)...), alloc_version());
      }
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
   iterator emplace(const_iterator position, Args && ...args)
   {
      //Just call more general insert(pos, size, value) and return iterator
      typedef container_detail::insert_emplace_proxy<Allocator, T*, Args...> type;
      return this->priv_forward_range_insert( vector_iterator_get_ptr(position), 1
                                            , type(::boost::forward<Args>(args)...), alloc_version());
   }

   #else

   #define BOOST_PP_LOCAL_MACRO(n)                                                                    \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)             \
   void emplace_back(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                              \
   {                                                                                                  \
      T* back_pos = container_detail::to_raw_pointer                                                  \
         (this->m_holder.start()) + this->m_holder.m_size;                                            \
      if (this->m_holder.m_size < this->m_holder.capacity()){                                         \
         allocator_traits_type::construct (this->m_holder.alloc()                                     \
            , back_pos BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) );              \
         ++this->m_holder.m_size;                                                                     \
      }                                                                                               \
      else{                                                                                           \
         typedef container_detail::BOOST_PP_CAT(insert_emplace_proxy_arg, n)                          \
            <Allocator, T* BOOST_PP_ENUM_TRAILING_PARAMS(n, P)> type;                                 \
         this->priv_forward_range_insert_no_capacity                                                  \
            ( vector_iterator_get_ptr(this->cend()), 1                                                \
            , type(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)), alloc_version());          \
      }                                                                                               \
   }                                                                                                  \
                                                                                                      \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)             \
   iterator emplace(const_iterator pos                                                                \
                    BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                      \
   {                                                                                                  \
      typedef container_detail::BOOST_PP_CAT(insert_emplace_proxy_arg, n)                             \
         <Allocator, T* BOOST_PP_ENUM_TRAILING_PARAMS(n, P)> type;                                    \
      return this->priv_forward_range_insert                                                          \
         ( container_detail::to_raw_pointer(vector_iterator_get_ptr(pos)), 1                          \
         , type(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)), alloc_version());             \
   }                                                                                                  \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

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
   iterator insert(const_iterator p, size_type n, const T& x)
   {
      container_detail::insert_n_copies_proxy<Allocator, T*> proxy(x);
      return this->priv_forward_range_insert(vector_iterator_get_ptr(p), n, proxy, alloc_version());
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
   //! <b>Complexity</b>: Linear to std::distance [first, last).
   template <class InIt>
   iterator insert(const_iterator pos, InIt first, InIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InIt, size_type>::value
            && container_detail::is_input_iterator<InIt>::value
         >::type * = 0
      #endif
      )
   {
      const size_type n_pos = pos - this->cbegin();
      iterator it(vector_iterator_get_ptr(pos));
      for(;first != last; ++first){
         it = this->emplace(it, *first);
         ++it;
      }
      return iterator(this->m_holder.start() + n_pos);
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class FwdIt>
   iterator insert(const_iterator pos, FwdIt first, FwdIt last
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<FwdIt, size_type>::value
            && !container_detail::is_input_iterator<FwdIt>::value
         >::type * = 0
      )
   {
      container_detail::insert_range_proxy<Allocator, FwdIt, T*> proxy(first);
      return this->priv_forward_range_insert(vector_iterator_get_ptr(pos), std::distance(first, last), proxy, alloc_version());
   }
   #endif

   //! <b>Requires</b>: p must be a valid iterator of *this. num, must
   //!   be equal to std::distance(first, last)
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or pos if first == last.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InpIt throws or T's copy/move constructor/assignment throws.
   //!
   //! <b>Complexity</b>: Linear to std::distance [first, last).
   //!
   //! <b>Note</b>: This function avoids a linear operation to calculate std::distance[first, last)
   //!   for forward and bidirectional iterators, and a one by one insertion for input iterators. This is a
   //!   a non-standard extension.
   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class InIt>
   iterator insert(const_iterator pos, size_type num, InIt first, InIt last)
   {
      BOOST_ASSERT(container_detail::is_input_iterator<InIt>::value ||
                   num == static_cast<size_type>(std::distance(first, last)));
      container_detail::insert_range_proxy<Allocator, InIt, T*> proxy(first);
      return this->priv_forward_range_insert(vector_iterator_get_ptr(pos), num, proxy, alloc_version());
   }
   #endif

   //! <b>Effects</b>: Removes the last element from the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   void pop_back() BOOST_CONTAINER_NOEXCEPT
   {
      //Destroy last element
      --this->m_holder.m_size;
      this->priv_destroy(container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size);
   }

   //! <b>Effects</b>: Erases the element at position pos.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements between pos and the
   //!   last element. Constant if pos is the last element.
   iterator erase(const_iterator position)
   {
      T *const pos = container_detail::to_raw_pointer(vector_iterator_get_ptr(position));
      T *const beg = container_detail::to_raw_pointer(this->m_holder.start());
      //Move elements forward and destroy last
      this->priv_destroy(::boost::move(pos + 1, beg + this->m_holder.m_size, pos));
      --this->m_holder.m_size;
      return iterator(vector_iterator_get_ptr(position));
   }

   //! <b>Effects</b>: Erases the elements pointed by [first, last).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the distance between first and last
   //!   plus linear to the elements between pos and the last element.
   iterator erase(const_iterator first, const_iterator last)
   {
      if (first != last){
         T* const end_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
         T* const ptr = container_detail::to_raw_pointer(boost::move
            (container_detail::to_raw_pointer(vector_iterator_get_ptr(last))
            ,end_pos
            ,container_detail::to_raw_pointer(vector_iterator_get_ptr(first))
            ));
         const size_type destroyed = (end_pos - ptr);
         boost::container::destroy_alloc_n(this->get_stored_allocator(), ptr, destroyed);
         this->m_holder.m_size -= destroyed;
      }
      return iterator(vector_iterator_get_ptr(first));
   }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(vector& x) BOOST_CONTAINER_NOEXCEPT_IF((!container_detail::is_version<Allocator, 0>::value))
   {
      //Just swap internals in case of !allocator_v0. Otherwise, deep swap
      this->m_holder.swap(x.m_holder);
      //And now the allocator
      container_detail::bool_<allocator_traits_type::propagate_on_container_swap::value> flag;
      container_detail::swap_alloc(this->m_holder.alloc(), x.m_holder.alloc(), flag);
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear
   //!
   //! <b>Note</b>: Non-standard extension to support static_vector
   template<class OtherAllocator>
   void swap(vector<T, OtherAllocator> & x
            , typename container_detail::enable_if_c
                     < container_detail::is_version<OtherAllocator, 0>::value &&
                      !container_detail::is_same<OtherAllocator, allocator_type>::value >::type * = 0
            )
   {  this->m_holder.swap(x.m_holder); }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //! <b>Effects</b>: Erases all the elements of the vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   void clear() BOOST_CONTAINER_NOEXCEPT
   {  this->priv_destroy_all();  }

   //! <b>Effects</b>: Returns true if x and y are equal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator==(const vector& x, const vector& y)
   {  return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());  }

   //! <b>Effects</b>: Returns true if x and y are unequal
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator!=(const vector& x, const vector& y)
   {  return !(x == y); }

   //! <b>Effects</b>: Returns true if x is less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<(const vector& x, const vector& y)
   {  return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   //! <b>Effects</b>: Returns true if x is greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>(const vector& x, const vector& y)
   {  return y < x;  }

   //! <b>Effects</b>: Returns true if x is equal or less than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator<=(const vector& x, const vector& y)
   {  return !(y < x);  }

   //! <b>Effects</b>: Returns true if x is equal or greater than y
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the container.
   friend bool operator>=(const vector& x, const vector& y)
   {  return !(x < y);  }

   //! <b>Effects</b>: x.swap(y)
   //!
   //! <b>Complexity</b>: Constant.
   friend void swap(vector& x, vector& y)
   {  x.swap(y);  }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //Absolutely experimental. This function might change, disappear or simply crash!
   template<class BiDirPosConstIt, class BiDirValueIt>
   void insert_ordered_at(size_type element_count, BiDirPosConstIt last_position_it, BiDirValueIt last_value_it)
   {
      const size_type *dummy = 0;
      this->priv_insert_ordered_at(element_count, last_position_it, false, dummy, last_value_it);
   }

   //Absolutely experimental. This function might change, disappear or simply crash!
   template<class BiDirPosConstIt, class BiDirSkipConstIt, class BiDirValueIt>
   void insert_ordered_at( size_type element_count, BiDirPosConstIt last_position_it
                         , BiDirSkipConstIt last_skip_it, BiDirValueIt last_value_it)
   {
      this->priv_insert_ordered_at(element_count, last_position_it, true, last_skip_it, last_value_it);
   }

   private:

   template<class OtherAllocator>
   void priv_move_assign(BOOST_RV_REF_BEG vector<T, OtherAllocator> BOOST_RV_REF_END x
      , typename container_detail::enable_if_c
         < container_detail::is_version<OtherAllocator, 0>::value >::type * = 0)
   {
      if(!container_detail::is_same<OtherAllocator, allocator_type>::value &&
          this->capacity() < x.size()){
         throw_bad_alloc();
      }
      T* const this_start  = container_detail::to_raw_pointer(m_holder.start());
      T* const other_start = container_detail::to_raw_pointer(x.m_holder.start());
      const size_type this_sz  = m_holder.m_size;
      const size_type other_sz = static_cast<size_type>(x.m_holder.m_size);
      boost::container::move_assign_range_alloc_n(this->m_holder.alloc(), other_start, other_sz, this_start, this_sz);
      this->m_holder.m_size = other_sz;
   }

   template<class OtherAllocator>
   void priv_move_assign(BOOST_RV_REF_BEG vector<T, OtherAllocator> BOOST_RV_REF_END x
      , typename container_detail::enable_if_c
         < !container_detail::is_version<OtherAllocator, 0>::value &&
           container_detail::is_same<OtherAllocator, allocator_type>::value>::type * = 0)
   {
      //for move constructor, no aliasing (&x != this) is assummed.
      BOOST_ASSERT(this != &x);
      allocator_type &this_alloc = this->m_holder.alloc();
      allocator_type &x_alloc    = x.m_holder.alloc();
      const bool propagate_alloc = allocator_traits_type::
            propagate_on_container_move_assignment::value;
      container_detail::bool_<propagate_alloc> flag;
      const bool allocators_equal = this_alloc == x_alloc; (void)allocators_equal;
      //Resources can be transferred if both allocators are
      //going to be equal after this function (either propagated or already equal)
      if(propagate_alloc || allocators_equal){
         //Destroy objects but retain memory in case x reuses it in the future
         this->clear();
         //Move allocator if needed
         container_detail::move_alloc(this_alloc, x_alloc, flag);
         //Nothrow swap
         this->m_holder.swap(x.m_holder);
      }
      //Else do a one by one move
      else{
         this->assign( boost::make_move_iterator(x.begin())
                     , boost::make_move_iterator(x.end()));
      }
   }

   template<class OtherAllocator>
   void priv_copy_assign(const vector<T, OtherAllocator> &x
      , typename container_detail::enable_if_c
         < container_detail::is_version<OtherAllocator, 0>::value >::type * = 0)
   {
      if(!container_detail::is_same<OtherAllocator, allocator_type>::value &&
         this->capacity() < x.size()){
         throw_bad_alloc();
      }
      T* const this_start  = container_detail::to_raw_pointer(m_holder.start());
      T* const other_start = container_detail::to_raw_pointer(x.m_holder.start());
      const size_type this_sz  = m_holder.m_size;
      const size_type other_sz = static_cast<size_type>(x.m_holder.m_size);
      boost::container::copy_assign_range_alloc_n(this->m_holder.alloc(), other_start, other_sz, this_start, this_sz);
      this->m_holder.m_size = other_sz;
   }

   template<class OtherAllocator>
   void priv_copy_assign(const vector<T, OtherAllocator> &x
      , typename container_detail::enable_if_c
         < !container_detail::is_version<OtherAllocator, 0>::value &&
           container_detail::is_same<OtherAllocator, allocator_type>::value >::type * = 0)
   {
      allocator_type &this_alloc     = this->m_holder.alloc();
      const allocator_type &x_alloc  = x.m_holder.alloc();
      container_detail::bool_<allocator_traits_type::
         propagate_on_container_copy_assignment::value> flag;
      if(flag && this_alloc != x_alloc){
         this->clear();
         this->shrink_to_fit();
      }
      container_detail::assign_alloc(this_alloc, x_alloc, flag);
      this->assign( container_detail::to_raw_pointer(x.m_holder.start())
                  , container_detail::to_raw_pointer(x.m_holder.start() + x.m_holder.m_size));
   }

   void priv_reserve(size_type, allocator_v0)
   {  throw_bad_alloc();  }

   container_detail::insert_range_proxy<Allocator, boost::move_iterator<T*>, T*> priv_dummy_empty_proxy()
   {
      return container_detail::insert_range_proxy<Allocator, boost::move_iterator<T*>, T*>
         (::boost::make_move_iterator((T *)0));
   }

   void priv_reserve(size_type new_cap, allocator_v1)
   {
      //There is not enough memory, allocate a new buffer
      pointer p = this->m_holder.allocate(new_cap);
      //We will reuse insert code, so create a dummy input iterator
      this->priv_forward_range_insert_new_allocation
         ( container_detail::to_raw_pointer(p), new_cap
         , container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size
         , 0, this->priv_dummy_empty_proxy());
   }

   void priv_reserve(size_type new_cap, allocator_v2)
   {
      //There is not enough memory, allocate a new
      //buffer or expand the old one.
      bool same_buffer_start;
      size_type real_cap = 0;
      std::pair<pointer, bool> ret = this->m_holder.allocation_command
         (allocate_new | expand_fwd | expand_bwd, new_cap, new_cap, real_cap, this->m_holder.start());

      //Check for forward expansion
      same_buffer_start = ret.second && this->m_holder.start() == ret.first;
      if(same_buffer_start){
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_expand_fwd;
         #endif
         this->m_holder.capacity(real_cap);
      }
      else{ //If there is no forward expansion, move objects, we will reuse insertion code
         T * const new_mem = container_detail::to_raw_pointer(ret.first);
         T * const ins_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
         if(ret.second){   //Backwards (and possibly forward) expansion
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_bwd;
            #endif
            this->priv_forward_range_insert_expand_backwards
               ( new_mem , real_cap, ins_pos, 0, this->priv_dummy_empty_proxy());
         }
         else{ //New buffer
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_alloc;
            #endif
            this->priv_forward_range_insert_new_allocation
               ( new_mem, real_cap, ins_pos, 0, this->priv_dummy_empty_proxy());
         }
      }
   }

   void priv_destroy(value_type* p) BOOST_CONTAINER_NOEXCEPT
   {
      if(!value_traits::trivial_dctr)
         allocator_traits_type::destroy(this->get_stored_allocator(), p);
   }

   void priv_destroy_last_n(size_type n) BOOST_CONTAINER_NOEXCEPT
   {
      T* const end_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
      boost::container::destroy_alloc_n(this->get_stored_allocator(), end_pos-n, n);
      this->m_holder.m_size -= n;
   }

   template<class InpIt>
   void priv_uninitialized_construct_at_end(InpIt first, InpIt last)
   {
      T* end_pos = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
      for(; first != last; ++first, ++end_pos, ++this->m_holder.m_size){
         //There is more memory, just construct a new object at the end
         allocator_traits_type::construct(this->m_holder.alloc(), end_pos, *first);
      }
   }

   void priv_destroy_all() BOOST_CONTAINER_NOEXCEPT
   {
      boost::container::destroy_alloc_n
         (this->get_stored_allocator(), container_detail::to_raw_pointer(this->m_holder.start()), this->m_holder.m_size);
      this->m_holder.m_size = 0;
   }

   template<class U>
   iterator priv_insert(const const_iterator &p, BOOST_FWD_REF(U) x)
   {
      return this->priv_forward_range_insert
         ( vector_iterator_get_ptr(p), 1, container_detail::get_insert_value_proxy<T*, Allocator>
            (::boost::forward<U>(x)), alloc_version());
   }

   container_detail::insert_copy_proxy<Allocator, T*> priv_single_insert_proxy(const T &x)
   {  return container_detail::insert_copy_proxy<Allocator, T*> (x);  }

   container_detail::insert_move_proxy<Allocator, T*> priv_single_insert_proxy(BOOST_RV_REF(T) x)
   {  return container_detail::insert_move_proxy<Allocator, T*> (x);  }

   template <class U>
   void priv_push_back(BOOST_FWD_REF(U) u)
   {
      if (this->m_holder.m_size < this->m_holder.capacity()){
         //There is more memory, just construct a new object at the end
         allocator_traits_type::construct
            ( this->m_holder.alloc()
            , container_detail::to_raw_pointer(this->m_holder.start() + this->m_holder.m_size)
            , ::boost::forward<U>(u) );
         ++this->m_holder.m_size;
      }
      else{
         this->priv_forward_range_insert_no_capacity
            ( vector_iterator_get_ptr(this->cend()), 1
            , this->priv_single_insert_proxy(::boost::forward<U>(u)), alloc_version());
      }
   }

   container_detail::insert_n_copies_proxy<Allocator, T*> priv_resize_proxy(const T &x)
   {  return container_detail::insert_n_copies_proxy<Allocator, T*>(x);   }

   container_detail::insert_default_initialized_n_proxy<Allocator, T*> priv_resize_proxy(default_init_t)
   {  return container_detail::insert_default_initialized_n_proxy<Allocator, T*>();  }

   container_detail::insert_value_initialized_n_proxy<Allocator, T*> priv_resize_proxy(value_init_t)
   {  return container_detail::insert_value_initialized_n_proxy<Allocator, T*>(); }

   template <class U>
   void priv_resize(size_type new_size, const U& u)
   {
      const size_type sz = this->size();
      if (new_size < sz){
         //Destroy last elements
         this->priv_destroy_last_n(sz - new_size);
      }
      else{
         const size_type n = new_size - this->size();
         this->priv_forward_range_insert_at_end(n, this->priv_resize_proxy(u), alloc_version());
      }
   }

   void priv_shrink_to_fit(allocator_v0) BOOST_CONTAINER_NOEXCEPT
   {}

   void priv_shrink_to_fit(allocator_v1)
   {
      const size_type cp = this->m_holder.capacity();
      if(cp){
         const size_type sz = this->size();
         if(!sz){
            this->m_holder.alloc().deallocate(this->m_holder.m_start, cp);
            this->m_holder.m_start     = pointer();
            this->m_holder.m_capacity  = 0;
         }
         else if(sz < cp){
            //Allocate a new buffer.
            pointer p = this->m_holder.allocate(sz);

            //We will reuse insert code, so create a dummy input iterator
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_alloc;
            #endif
            this->priv_forward_range_insert_new_allocation
               ( container_detail::to_raw_pointer(p), sz
               , container_detail::to_raw_pointer(this->m_holder.start())
               , 0, this->priv_dummy_empty_proxy());
         }
      }
   }

   void priv_shrink_to_fit(allocator_v2) BOOST_CONTAINER_NOEXCEPT
   {
      const size_type cp = this->m_holder.capacity();
      if(cp){
         const size_type sz = this->size();
         if(!sz){
            this->m_holder.alloc().deallocate(this->m_holder.m_start, cp);
            this->m_holder.m_start     = pointer();
            this->m_holder.m_capacity  = 0;
         }
         else{
            size_type received_size;
            if(this->m_holder.allocation_command
               ( shrink_in_place | nothrow_allocation
               , cp, sz, received_size, this->m_holder.start()).first){
               this->m_holder.capacity(received_size);
               #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
               ++this->num_shrink;
               #endif
            }
         }
      }
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_no_capacity
      (const pointer &pos, const size_type, const InsertionProxy , allocator_v0)
   {
      throw_bad_alloc();
      return iterator(pos);
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_no_capacity
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy, allocator_v1)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type n_pos = pos - this->m_holder.start();
      T *const raw_pos = container_detail::to_raw_pointer(pos);

      const size_type new_cap = this->m_holder.next_capacity(n);
      T * new_buf = container_detail::to_raw_pointer(this->m_holder.alloc().allocate(new_cap));
      #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
      ++this->num_alloc;
      #endif
      this->priv_forward_range_insert_new_allocation
         ( new_buf, new_cap, raw_pos, n, insert_range_proxy);
      return iterator(this->m_holder.start() + n_pos);
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_no_capacity
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy, allocator_v2)
   {
      //Check if we have enough memory or try to expand current memory
      T *const raw_pos = container_detail::to_raw_pointer(pos);
      const size_type n_pos = raw_pos - container_detail::to_raw_pointer(this->m_holder.start());

      size_type real_cap = 0;
      //There is not enough memory, allocate a new
      //buffer or expand the old one.
      std::pair<pointer, bool> ret = (this->m_holder.allocation_command
            (allocate_new | expand_fwd | expand_bwd,
               this->m_holder.m_size + n, this->m_holder.next_capacity(n), real_cap, this->m_holder.start()));

      //Buffer reallocated
      if(ret.second){
         //Forward expansion, delay insertion
         if(this->m_holder.start() == ret.first){
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_fwd;
            #endif
            this->m_holder.capacity(real_cap);
            //Expand forward
            this->priv_forward_range_insert_expand_forward(raw_pos, n, insert_range_proxy);
         }
         //Backwards (and possibly forward) expansion
         else{
            #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
            ++this->num_expand_bwd;
            #endif
            this->priv_forward_range_insert_expand_backwards
               ( container_detail::to_raw_pointer(ret.first)
               , real_cap, raw_pos, n, insert_range_proxy);
         }
      }
      //New buffer
      else{
         #ifdef BOOST_CONTAINER_VECTOR_ALLOC_STATS
         ++this->num_alloc;
         #endif
         this->priv_forward_range_insert_new_allocation
            ( container_detail::to_raw_pointer(ret.first)
            , real_cap, raw_pos, n, insert_range_proxy);
      }

      return iterator(this->m_holder.start() + n_pos);
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy, allocator_v0)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type remaining = this->m_holder.capacity() - this->m_holder.m_size;

      if (n > remaining){
         //This will trigger an error
         throw_bad_alloc();
      }
      const size_type n_pos = pos - this->m_holder.start();
      T *const raw_pos = container_detail::to_raw_pointer(pos);
      this->priv_forward_range_insert_expand_forward(raw_pos, n, insert_range_proxy);
      return iterator(this->m_holder.start() + n_pos);
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy, allocator_v1)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type remaining = this->m_holder.capacity() - this->m_holder.m_size;
      T *const raw_pos = container_detail::to_raw_pointer(pos);

      if (n <= remaining){
         const size_type n_pos = raw_pos - container_detail::to_raw_pointer(this->m_holder.start());
         this->priv_forward_range_insert_expand_forward(raw_pos, n, insert_range_proxy);
         return iterator(this->m_holder.start() + n_pos);
      }
      else{
         return this->priv_forward_range_insert_no_capacity(pos, n, insert_range_proxy, alloc_version());
      }
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert
      (const pointer &pos, const size_type n, const InsertionProxy insert_range_proxy, allocator_v2)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type remaining = this->m_holder.capacity() - this->m_holder.m_size;

      bool same_buffer_start = n <= remaining;
      if (!same_buffer_start){
         return priv_forward_range_insert_no_capacity(pos, n, insert_range_proxy, alloc_version());
      }
      else{
         //Expand forward
         T *const raw_pos = container_detail::to_raw_pointer(pos);
         const size_type n_pos = raw_pos - container_detail::to_raw_pointer(this->m_holder.start());
         this->priv_forward_range_insert_expand_forward(raw_pos, n, insert_range_proxy);
         return iterator(this->m_holder.start() + n_pos);
      }
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_at_end
      (const size_type n, const InsertionProxy insert_range_proxy, allocator_v0)
   {
      //Check if we have enough memory or try to expand current memory
      const size_type remaining = this->m_holder.capacity() - this->m_holder.m_size;

      if (n > remaining){
         //This will trigger an error
         throw_bad_alloc();
      }
      this->priv_forward_range_insert_at_end_expand_forward(n, insert_range_proxy);
      return this->end();
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_at_end
      (const size_type n, const InsertionProxy insert_range_proxy, allocator_v1)
   {
      return this->priv_forward_range_insert(vector_iterator_get_ptr(this->cend()), n, insert_range_proxy, allocator_v1());
   }

   template <class InsertionProxy>
   iterator priv_forward_range_insert_at_end
      (const size_type n, const InsertionProxy insert_range_proxy, allocator_v2)
   {
      return this->priv_forward_range_insert(vector_iterator_get_ptr(this->cend()), n, insert_range_proxy, allocator_v2());
   }

   //Absolutely experimental. This function might change, disappear or simply crash!
   template<class BiDirPosConstIt, class BiDirSkipConstIt, class BiDirValueIt>
   void priv_insert_ordered_at( size_type element_count, BiDirPosConstIt last_position_it
                              , bool do_skip, BiDirSkipConstIt last_skip_it, BiDirValueIt last_value_it)
   {
      const size_type old_size_pos = this->size();
      this->reserve(old_size_pos + element_count);
      T* const begin_ptr = container_detail::to_raw_pointer(this->m_holder.start());
      size_type insertions_left = element_count;
      size_type next_pos = old_size_pos;
      size_type hole_size = element_count;

      //Exception rollback. If any copy throws before the hole is filled, values
      //already inserted/copied at the end of the buffer will be destroyed.
      typename value_traits::ArrayDestructor past_hole_values_destroyer
         (begin_ptr + old_size_pos + element_count, this->m_holder.alloc(), size_type(0u));
      //Loop for each insertion backwards, first moving the elements after the insertion point,
      //then inserting the element.
      while(insertions_left){
         if(do_skip){
            size_type n = *(--last_skip_it);
            std::advance(last_value_it, -difference_type(n));
         }
         const size_type pos = static_cast<size_type>(*(--last_position_it));
         BOOST_ASSERT(pos <= old_size_pos);
         //If needed shift the range after the insertion point and the previous insertion point.
         //Function will take care if the shift crosses the size() boundary, using copy/move
         //or uninitialized copy/move if necessary.
         size_type new_hole_size = (pos != next_pos)
            ? priv_insert_ordered_at_shift_range(pos, next_pos, this->size(), insertions_left)
            : hole_size
            ;
         if(new_hole_size > 0){
            //The hole was reduced by priv_insert_ordered_at_shift_range so expand exception rollback range backwards
            past_hole_values_destroyer.increment_size_backwards(next_pos - pos);
            //Insert the new value in the hole
            allocator_traits_type::construct(this->m_holder.alloc(), begin_ptr + pos + insertions_left - 1, *(--last_value_it));
            --new_hole_size;
            if(new_hole_size == 0){
               //Hole was just filled, disable exception rollback and change vector size
               past_hole_values_destroyer.release();
               this->m_holder.m_size += element_count;
            }
            else{
               //The hole was reduced by the new insertion by one
               past_hole_values_destroyer.increment_size_backwards(size_type(1u));
            }
         }
         else{
            if(hole_size){
               //Hole was just filled by priv_insert_ordered_at_shift_range, disable exception rollback and change vector size
               past_hole_values_destroyer.release();
               this->m_holder.m_size += element_count;
            }
            //Insert the new value in the already constructed range
            begin_ptr[pos + insertions_left - 1] = *(--last_value_it);
         }
         --insertions_left;
         hole_size = new_hole_size;
         next_pos = pos;
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
   //New situation in Case Allocator (hole_size == 0):
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
   //New situation in Case B (hole_size > 0):
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
      T* const begin_ptr = container_detail::to_raw_pointer(this->m_holder.start());
      T* const first_ptr = begin_ptr + first_pos;
      T* const last_ptr  = begin_ptr + last_pos;

      size_type hole_size = 0;
      //Case A:
      if((last_pos + shift_count) <= limit_pos){
         //All move assigned
         boost::move_backward(first_ptr, last_ptr, last_ptr + shift_count);
      }
      //Case B:
      else if((first_pos + shift_count) >= limit_pos){
         //All uninitialized_moved
         ::boost::container::uninitialized_move_alloc
            (this->m_holder.alloc(), first_ptr, last_ptr, first_ptr + shift_count);
         hole_size = last_pos + shift_count - limit_pos;
      }
      //Case C:
      else{
         //Some uninitialized_moved
         T* const limit_ptr    = begin_ptr + limit_pos;
         T* const boundary_ptr = limit_ptr - shift_count;
         ::boost::container::uninitialized_move_alloc(this->m_holder.alloc(), boundary_ptr, last_ptr, limit_ptr);
         //The rest is move assigned
         boost::move_backward(first_ptr, boundary_ptr, limit_ptr);
      }
      return hole_size;
   }

   private:
   template <class InsertionProxy>
   void priv_forward_range_insert_at_end_expand_forward(const size_type n, InsertionProxy insert_range_proxy)
   {
      T* const old_finish = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
      insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, n);
      this->m_holder.m_size += n;
   }

   template <class InsertionProxy>
   void priv_forward_range_insert_expand_forward(T* const pos, const size_type n, InsertionProxy insert_range_proxy)
   {
      //n can't be 0, because there is nothing to do in that case
      if(!n) return;
      //There is enough memory
      T* const old_finish = container_detail::to_raw_pointer(this->m_holder.start()) + this->m_holder.m_size;
      const size_type elems_after = old_finish - pos;

      if (!elems_after){
         insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, n);
         this->m_holder.m_size += n;
      }
      else if (elems_after >= n){
         //New elements can be just copied.
         //Move to uninitialized memory last objects
         ::boost::container::uninitialized_move_alloc
            (this->m_holder.alloc(), old_finish - n, old_finish, old_finish);
         this->m_holder.m_size += n;
         //Copy previous to last objects to the initialized end
         boost::move_backward(pos, old_finish - n, old_finish);
         //Insert new objects in the pos
         insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), pos, n);
      }
      else {
         //The new elements don't fit in the [pos, end()) range.

         //Copy old [pos, end()) elements to the uninitialized memory (a gap is created)
         ::boost::container::uninitialized_move_alloc(this->m_holder.alloc(), pos, old_finish, pos + n);
         BOOST_TRY{
            //Copy first new elements in pos (gap is still there)
            insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), pos, elems_after);
            //Copy to the beginning of the unallocated zone the last new elements (the gap is closed).
            insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, n - elems_after);
            this->m_holder.m_size += n;
         }
         BOOST_CATCH(...){
            boost::container::destroy_alloc_n(this->get_stored_allocator(), pos + n, elems_after);
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
   }

   template <class InsertionProxy>
   void priv_forward_range_insert_new_allocation
      (T* const new_start, size_type new_cap, T* const pos, const size_type n, InsertionProxy insert_range_proxy)
   {
      //n can be zero, if we want to reallocate!
      T *new_finish = new_start;
      T *old_finish;
      //Anti-exception rollbacks
      typename value_traits::ArrayDeallocator new_buffer_deallocator(new_start, this->m_holder.alloc(), new_cap);
      typename value_traits::ArrayDestructor  new_values_destroyer(new_start, this->m_holder.alloc(), 0u);

      //Initialize with [begin(), pos) old buffer
      //the start of the new buffer
      T * const old_buffer = container_detail::to_raw_pointer(this->m_holder.start());
      if(old_buffer){
         new_finish = ::boost::container::uninitialized_move_alloc
            (this->m_holder.alloc(), container_detail::to_raw_pointer(this->m_holder.start()), pos, old_finish = new_finish);
         new_values_destroyer.increment_size(new_finish - old_finish);
      }
      //Initialize new objects, starting from previous point
      old_finish = new_finish;
      insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, n);
      new_finish += n;
      new_values_destroyer.increment_size(new_finish - old_finish);
      //Initialize from the rest of the old buffer,
      //starting from previous point
      if(old_buffer){
         new_finish = ::boost::container::uninitialized_move_alloc
            (this->m_holder.alloc(), pos, old_buffer + this->m_holder.m_size, new_finish);
         //Destroy and deallocate old elements
         //If there is allocated memory, destroy and deallocate
         if(!value_traits::trivial_dctr_after_move)
            boost::container::destroy_alloc_n(this->get_stored_allocator(), old_buffer, this->m_holder.m_size);
         this->m_holder.alloc().deallocate(this->m_holder.start(), this->m_holder.capacity());
      }
      this->m_holder.start(new_start);
      this->m_holder.m_size = new_finish - new_start;
      this->m_holder.capacity(new_cap);
      //All construction successful, disable rollbacks
      new_values_destroyer.release();
      new_buffer_deallocator.release();
   }

   template <class InsertionProxy>
   void priv_forward_range_insert_expand_backwards
         (T* const new_start, const size_type new_capacity,
          T* const pos, const size_type n, InsertionProxy insert_range_proxy)
   {
      //n can be zero to just expand capacity
      //Backup old data
      T* const old_start  = container_detail::to_raw_pointer(this->m_holder.start());
      const size_type old_size = this->m_holder.m_size;
      T* const old_finish = old_start + old_size;

      //We can have 8 possibilities:
      const size_type elemsbefore = static_cast<size_type>(pos - old_start);
      const size_type s_before    = static_cast<size_type>(old_start - new_start);
      const size_type before_plus_new = elemsbefore + n;

      //Update the vector buffer information to a safe state
      this->m_holder.start(new_start);
      this->m_holder.capacity(new_capacity);
      this->m_holder.m_size = 0;

      //If anything goes wrong, this object will destroy
      //all the old objects to fulfill previous vector state
      typename value_traits::ArrayDestructor old_values_destroyer(old_start, this->m_holder.alloc(), old_size);
      //Check if s_before is big enough to hold the beginning of old data + new data
      if(s_before >= before_plus_new){
         //Copy first old values before pos, after that the new objects
         T *const new_elem_pos =
            ::boost::container::uninitialized_move_alloc(this->m_holder.alloc(), old_start, pos, new_start);
         this->m_holder.m_size = elemsbefore;
         insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), new_elem_pos, n);
         this->m_holder.m_size = before_plus_new;
         const size_type new_size = old_size + n;
         //Check if s_before is so big that even copying the old data + new data
         //there is a gap between the new data and the old data
         if(s_before >= new_size){
            //Old situation:
            // _________________________________________________________
            //|            raw_mem                | old_begin | old_end |
            //| __________________________________|___________|_________|
            //
            //New situation:
            // _________________________________________________________
            //| old_begin |    new   | old_end |         raw_mem        |
            //|___________|__________|_________|________________________|
            //
            //Now initialize the rest of memory with the last old values
            if(before_plus_new != new_size){ //Special case to avoid operations in back insertion
               ::boost::container::uninitialized_move_alloc
                  (this->m_holder.alloc(), pos, old_finish, new_start + before_plus_new);
               //All new elements correctly constructed, avoid new element destruction
               this->m_holder.m_size = new_size;
            }
            //Old values destroyed automatically with "old_values_destroyer"
            //when "old_values_destroyer" goes out of scope unless the have trivial
            //destructor after move.
            if(value_traits::trivial_dctr_after_move)
               old_values_destroyer.release();
         }
         //s_before is so big that divides old_end
         else{
            //Old situation:
            // __________________________________________________
            //|            raw_mem         | old_begin | old_end |
            //| ___________________________|___________|_________|
            //
            //New situation:
            // __________________________________________________
            //| old_begin |   new    | old_end |  raw_mem        |
            //|___________|__________|_________|_________________|
            //
            //Now initialize the rest of memory with the last old values
            //All new elements correctly constructed, avoid new element destruction
            const size_type raw_gap = s_before - before_plus_new;
            if(!value_traits::trivial_dctr){
               //Now initialize the rest of s_before memory with the
               //first of elements after new values
               ::boost::container::uninitialized_move_alloc_n
                  (this->m_holder.alloc(), pos, raw_gap, new_start + before_plus_new);
               //Now we have a contiguous buffer so program trailing element destruction
               //and update size to the final size.
               old_values_destroyer.shrink_forward(elemsbefore + raw_gap);
               this->m_holder.m_size = new_size;
               //Now move remaining last objects in the old buffer begin
               ::boost::move(pos + raw_gap, old_finish, old_start);
               //Once moved, avoid calling the destructors if trivial after move
               if(value_traits::trivial_dctr_after_move){
                  old_values_destroyer.release();
               }
            }
            else{ //If trivial destructor, we can uninitialized copy + copy in a single uninitialized copy
               ::boost::container::uninitialized_move_alloc_n
                  (this->m_holder.alloc(), pos, old_finish - pos, new_start + before_plus_new);
               this->m_holder.m_size = new_size;
               old_values_destroyer.release();
            }
         }
      }
      else{
         //Check if we have to do the insertion in two phases
         //since maybe s_before is not big enough and
         //the buffer was expanded both sides
         //
         //Old situation:
         // _________________________________________________
         //| raw_mem | old_begin + old_end |  raw_mem        |
         //|_________|_____________________|_________________|
         //
         //New situation with do_after:
         // _________________________________________________
         //|     old_begin + new + old_end     |  raw_mem    |
         //|___________________________________|_____________|
         //
         //New without do_after:
         // _________________________________________________
         //| old_begin + new + old_end  |  raw_mem           |
         //|____________________________|____________________|
         //
         const bool do_after = n > s_before;

         //Now we can have two situations: the raw_mem of the
         //beginning divides the old_begin, or the new elements:
         if (s_before <= elemsbefore) {
            //The raw memory divides the old_begin group:
            //
            //If we need two phase construction (do_after)
            //new group is divided in new = new_beg + new_end groups
            //In this phase only new_beg will be inserted
            //
            //Old situation:
            // _________________________________________________
            //| raw_mem | old_begin | old_end |  raw_mem        |
            //|_________|___________|_________|_________________|
            //
            //New situation with do_after(1):
            //This is not definitive situation, the second phase
            //will include
            // _________________________________________________
            //| old_begin | new_beg | old_end |  raw_mem        |
            //|___________|_________|_________|_________________|
            //
            //New situation without do_after:
            // _________________________________________________
            //| old_begin | new | old_end |  raw_mem            |
            //|___________|_____|_________|_____________________|
            //
            //Copy the first part of old_begin to raw_mem
            ::boost::container::uninitialized_move_alloc_n
               (this->m_holder.alloc(), old_start, s_before, new_start);
            //The buffer is all constructed until old_end
            if(do_after){
               //release destroyer and update size
               old_values_destroyer.release();
               this->m_holder.m_size = old_size + s_before;
               //Now copy the second part of old_begin overwriting itself
               T *const next = ::boost::move(old_start + s_before, pos, old_start);
               //Now copy the new_beg elements
               insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), next, s_before);
            }
            else{
               //The buffer is all constructed until old_end,
               //so program trailing destruction and assign final size
               this->m_holder.m_size = old_size + n;
               const size_type n_destroy = s_before - n;
               old_values_destroyer.shrink_forward(old_size - n_destroy);
               //Now copy the second part of old_begin overwriting itself
               T *const next = ::boost::move(old_start + s_before, pos, old_start);
               //Now copy the all the new elements
               insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), next, n);
               //Now displace old_end elements
               ::boost::move(pos, old_finish, next + n);
               if(value_traits::trivial_dctr_after_move)
                  old_values_destroyer.release();
            }
         }
         else {
            //If we have to expand both sides,
            //we will play if the first new values so
            //calculate the upper bound of new values

            //The raw memory divides the new elements
            //
            //If we need two phase construction (do_after)
            //new group is divided in new = new_beg + new_end groups
            //In this phase only new_beg will be inserted
            //
            //Old situation:
            // _______________________________________________________
            //|   raw_mem     | old_begin | old_end |  raw_mem        |
            //|_______________|___________|_________|_________________|
            //
            //New situation with do_after():
            // ____________________________________________________
            //| old_begin |    new_beg    | old_end |  raw_mem     |
            //|___________|_______________|_________|______________|
            //
            //New situation without do_after:
            // ______________________________________________________
            //| old_begin | new | old_end |  raw_mem                 |
            //|___________|_____|_________|__________________________|
            //
            //First copy whole old_begin and part of new to raw_mem
            T * const new_pos = ::boost::container::uninitialized_move_alloc
               (this->m_holder.alloc(), old_start, pos, new_start);
            this->m_holder.m_size = elemsbefore;
            const size_type mid_n = s_before - elemsbefore;
            insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), new_pos, mid_n);
            //The buffer is all constructed until old_end,
            //release destroyer
            this->m_holder.m_size = old_size + s_before;
            old_values_destroyer.release();

            if(do_after){
               //Copy new_beg part
               insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), old_start, elemsbefore);
            }
            else{
               //Copy all new elements
               const size_type rest_new = n - mid_n;
               insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), old_start, rest_new);
               T* const move_start = old_start + rest_new;
               //Displace old_end
               T* const move_end = ::boost::move(pos, old_finish, move_start);
               //Destroy remaining moved elements from old_end except if they
               //have trivial destructor after being moved
               size_type n_destroy = s_before - n;
               if(!value_traits::trivial_dctr_after_move)
                  boost::container::destroy_alloc_n(this->get_stored_allocator(), move_end, n_destroy);
               this->m_holder.m_size -= n_destroy;
            }
         }

         //This is only executed if two phase construction is needed
         if(do_after){
            //The raw memory divides the new elements
            //
            //Old situation:
            // ______________________________________________________
            //|   raw_mem    | old_begin |  old_end   |  raw_mem     |
            //|______________|___________|____________|______________|
            //
            //New situation with do_after(1):
            // _______________________________________________________
            //| old_begin   +   new_beg  | new_end |old_end | raw_mem |
            //|__________________________|_________|________|_________|
            //
            //New situation with do_after(2):
            // ______________________________________________________
            //| old_begin      +       new            | old_end |raw |
            //|_______________________________________|_________|____|
            //
            const size_type n_after    = n - s_before;
            const size_type elemsafter = old_size - elemsbefore;

            //We can have two situations:
            if (elemsafter >= n_after){
               //The raw_mem from end will divide displaced old_end
               //
               //Old situation:
               // ______________________________________________________
               //|   raw_mem    | old_begin |  old_end   |  raw_mem     |
               //|______________|___________|____________|______________|
               //
               //New situation with do_after(1):
               // _______________________________________________________
               //| old_begin   +   new_beg  | new_end |old_end | raw_mem |
               //|__________________________|_________|________|_________|
               //
               //First copy the part of old_end raw_mem
               T* finish_n = old_finish - n_after;
               ::boost::container::uninitialized_move_alloc
                  (this->m_holder.alloc(), finish_n, old_finish, old_finish);
               this->m_holder.m_size += n_after;
               //Displace the rest of old_end to the new position
               boost::move_backward(pos, finish_n, old_finish);
               //Now overwrite with new_end
               //The new_end part is [first + (n - n_after), last)
               insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), pos, n_after);
            }
            else {
               //The raw_mem from end will divide new_end part
               //
               //Old situation:
               // _____________________________________________________________
               //|   raw_mem    | old_begin |  old_end   |  raw_mem            |
               //|______________|___________|____________|_____________________|
               //
               //New situation with do_after(2):
               // _____________________________________________________________
               //| old_begin   +   new_beg  |     new_end   |old_end | raw_mem |
               //|__________________________|_______________|________|_________|
               //

               const size_type mid_last_dist = n_after - elemsafter;
               //First initialize data in raw memory

               //Copy to the old_end part to the uninitialized zone leaving a gap.
               ::boost::container::uninitialized_move_alloc
                  (this->m_holder.alloc(), pos, old_finish, old_finish + mid_last_dist);

               BOOST_TRY{
                  //Copy the first part to the already constructed old_end zone
                  insert_range_proxy.copy_n_and_update(this->m_holder.alloc(), pos, elemsafter);
                  //Copy the rest to the uninitialized zone filling the gap
                  insert_range_proxy.uninitialized_copy_n_and_update(this->m_holder.alloc(), old_finish, mid_last_dist);
                  this->m_holder.m_size += n_after;
               }
               BOOST_CATCH(...){
                  boost::container::destroy_alloc_n(this->get_stored_allocator(), pos, mid_last_dist);
                  BOOST_RETHROW
               }
               BOOST_CATCH_END
/*
               size_type mid_last_dist = n_after - elemsafter;
               //First initialize data in raw memory

               //The new_end part is [first + (n - n_after), last)
               insert_range_proxy.uninitialized_copy_last_and_update(old_finish, elemsafter);
               this->m_holder.m_size += mid_last_dist;
               ::boost::container::uninitialized_move_alloc
                  (this->m_holder.alloc(), pos, old_finish, old_finish + mid_last_dist);
               this->m_holder.m_size += n_after - mid_last_dist;
               //Now copy the part of new_end over constructed elements
               insert_range_proxy.copy_remaining_to(pos);*/
            }
         }
      }
   }

   void priv_check_range(size_type n) const
   {
      //If n is out of range, throw an out_of_range exception
      if (n >= this->size()){
         throw_out_of_range("vector::at out of range");
      }
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

}}

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

namespace boost {


//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator>
struct has_trivial_destructor_after_move<boost::container::vector<T, Allocator> >
   : public ::boost::has_trivial_destructor_after_move<Allocator>
{};


}

//#define BOOST_CONTAINER_PUT_SWAP_OVERLOAD_IN_NAMESPACE_STD

#ifdef BOOST_CONTAINER_PUT_SWAP_OVERLOAD_IN_NAMESPACE_STD

namespace std {

template <class T, class Allocator>
inline void swap(boost::container::vector<T, Allocator>& x, boost::container::vector<T, Allocator>& y)
{  x.swap(y);  }

}  //namespace std {

#endif

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

#include <boost/container/detail/config_end.hpp>

#endif //   #ifndef  BOOST_CONTAINER_CONTAINER_VECTOR_HPP
