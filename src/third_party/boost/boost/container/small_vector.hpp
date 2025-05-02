//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2015-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_SMALL_VECTOR_HPP
#define BOOST_CONTAINER_CONTAINER_SMALL_VECTOR_HPP

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
#include <boost/container/vector.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/new_allocator.hpp> //new_allocator
// container/detail
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/version_type.hpp>

//move
#include <boost/move/adl_move_swap.hpp>
#include <boost/move/iterator.hpp>

//move/detail
#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
#include <boost/move/detail/fwd_macros.hpp>
#endif

//std
#if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
#include <initializer_list>   //for std::initializer_list
#endif

#include <cstddef> //offsetof

namespace boost {
namespace container {

namespace dtl{

template<class Options>
struct get_small_vector_opt
{
   typedef Options type;
};

template<>
struct get_small_vector_opt<void>
{
   typedef small_vector_null_opt type;
};

template<class Options>
struct get_vopt_from_svopt
   : get_small_vector_opt<Options>::type
{
   typedef typename get_small_vector_opt<Options>::type options_t;
   typedef vector_opt< typename options_t::growth_factor_type
                     , typename options_t::stored_size_type
                     > type;
};

template<>
struct get_vopt_from_svopt<void>
{
   typedef void type;
};

template <class T, class SecAlloc, class Options>
struct vector_for_small_vector
{
   typedef vector
      < T
      , small_vector_allocator
         < T 
         , typename allocator_traits<typename real_allocator<T, SecAlloc>::type>::template portable_rebind_alloc<void>::type
         , Options>
      , typename dtl::get_vopt_from_svopt<Options>::type
      > type;
};

}  //namespace dtl

//! A non-standard allocator used to implement `small_vector`.
//! Users should never use it directly. It is described here
//! for documentation purposes.
//! 
//! This allocator inherits from a standard-conforming allocator
//! and forwards member functions to the standard allocator except
//! when internal storage is being used as memory source.
//!
//! This allocator is a "partially_propagable" allocator and
//! defines `is_partially_propagable` as true_type.
//! 
//! A partially propagable allocator means that not all storage
//! allocatod by an instance of `small_vector_allocator` can be
//! deallocated by another instance of this type, even if both
//! instances compare equal or an instance is propagated to another
//! one using the copy/move constructor or assignment. The storage that
//! can never be propagated is identified by `storage_is_unpropagable(p)`.
//!
//! `boost::container::vector` supports partially propagable allocators
//! fallbacking to deep copy/swap/move operations when internal storage
//! is being used to store vector elements.
//!
//! `small_vector_allocator` assumes that will be instantiated as
//! `boost::container::vector< T, small_vector_allocator<T, Allocator> >`
//! and internal storage can be obtained downcasting that vector
//! to `small_vector_base<T>`.
template<class T, class VoidAlloc BOOST_CONTAINER_DOCONLY(= void), class Options BOOST_CONTAINER_DOCONLY(= void)>
class small_vector_allocator
   : public allocator_traits<VoidAlloc>::template portable_rebind_alloc<T>::type
{
   typedef unsigned int allocation_type;
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:

   typedef typename allocator_traits<VoidAlloc>::template portable_rebind_alloc<T>::type allocator_type;

   BOOST_COPYABLE_AND_MOVABLE(small_vector_allocator)

   inline const allocator_type &as_base() const BOOST_NOEXCEPT
   {  return static_cast<const allocator_type&>(*this);  }

   inline allocator_type &as_base() BOOST_NOEXCEPT
   {  return static_cast<allocator_type&>(*this);  }

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   typedef allocator_traits<allocator_type> allocator_traits_type;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   typedef typename allocator_traits<allocator_type>::value_type          value_type;
   typedef typename allocator_traits<allocator_type>::pointer             pointer;
   typedef typename allocator_traits<allocator_type>::const_pointer       const_pointer;
   typedef typename allocator_traits<allocator_type>::reference           reference;
   typedef typename allocator_traits<allocator_type>::const_reference     const_reference;
   typedef typename allocator_traits<allocator_type>::size_type           size_type;
   typedef typename allocator_traits<allocator_type>::difference_type     difference_type;
   typedef typename allocator_traits<allocator_type>::void_pointer        void_pointer;
   typedef typename allocator_traits<allocator_type>::const_void_pointer  const_void_pointer;

   typedef typename allocator_traits<allocator_type>::propagate_on_container_copy_assignment   propagate_on_container_copy_assignment;
   typedef typename allocator_traits<allocator_type>::propagate_on_container_move_assignment   propagate_on_container_move_assignment;
   typedef typename allocator_traits<allocator_type>::propagate_on_container_swap              propagate_on_container_swap;
   //! An integral constant with member `value == false`
   typedef BOOST_CONTAINER_IMPDEF(dtl::bool_<false>)                         is_always_equal;
   //! An integral constant with member `value == true`
   typedef BOOST_CONTAINER_IMPDEF(dtl::bool_<true>)                          is_partially_propagable;

   BOOST_CONTAINER_DOCIGN(typedef dtl::version_type<small_vector_allocator BOOST_CONTAINER_I 1>  version;)

   //!Obtains an small_vector_allocator that allocates
   //!objects of type T2
   template<class T2>
   struct rebind
   {
      typedef typename allocator_traits<allocator_type>::template portable_rebind_alloc<T2>::type other;
   };

   inline small_vector_allocator() BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
   {}

   //!Constructor from other small_vector_allocator.
   //!Never throws
   inline small_vector_allocator
      (const small_vector_allocator &other) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(other.as_base())
   {}

   //!Move constructor from small_vector_allocator.
   //!Never throws
   inline small_vector_allocator
      (BOOST_RV_REF(small_vector_allocator) other) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(::boost::move(other.as_base()))
   {}

   //!Constructor from related small_vector_allocator.
   //!Never throws
   template<class U, class OtherVoidAllocator, class OtherOptions>
   inline small_vector_allocator
      (const small_vector_allocator<U, OtherVoidAllocator, OtherOptions> &other) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(other.as_base())
   {}

   //!Move constructor from related small_vector_allocator.
   //!Never throws
   template<class U, class OtherVoidAllocator, class OtherOptions>
   inline small_vector_allocator
      (BOOST_RV_REF(small_vector_allocator<U BOOST_MOVE_I OtherVoidAllocator BOOST_MOVE_I OtherOptions>) other) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(::boost::move(other.as_base()))
   {}

   //!Constructor from allocator_type.
   //!Never throws
   inline explicit small_vector_allocator
      (const allocator_type &other) BOOST_NOEXCEPT_OR_NOTHROW
      : allocator_type(other)
   {}

   //!Assignment from other small_vector_allocator.
   //!Never throws
   inline small_vector_allocator &
      operator=(BOOST_COPY_ASSIGN_REF(small_vector_allocator) other) BOOST_NOEXCEPT_OR_NOTHROW
   {  return static_cast<small_vector_allocator&>(this->allocator_type::operator=(other.as_base()));  }

   //!Move assignment from other small_vector_allocator.
   //!Never throws
   inline small_vector_allocator &
      operator=(BOOST_RV_REF(small_vector_allocator) other) BOOST_NOEXCEPT_OR_NOTHROW
   {  return static_cast<small_vector_allocator&>(this->allocator_type::operator=(::boost::move(other.as_base())));  }

   //!Assignment from related small_vector_allocator.
   //!Never throws
   template<class U, class OtherVoidAllocator>
   inline small_vector_allocator &
      operator=(BOOST_COPY_ASSIGN_REF(small_vector_allocator<U BOOST_MOVE_I OtherVoidAllocator BOOST_MOVE_I Options>) other) BOOST_NOEXCEPT_OR_NOTHROW
   {  return static_cast<small_vector_allocator&>(this->allocator_type::operator=(other.as_base()));  }

   //!Move assignment from related small_vector_allocator.
   //!Never throws
   template<class U, class OtherVoidAllocator>
   inline small_vector_allocator &
      operator=(BOOST_RV_REF(small_vector_allocator<U BOOST_MOVE_I OtherVoidAllocator BOOST_MOVE_I Options>) other) BOOST_NOEXCEPT_OR_NOTHROW
   {  return static_cast<small_vector_allocator&>(this->allocator_type::operator=(::boost::move(other.as_base())));  }

   //!Move assignment from allocator_type.
   //!Never throws
   inline small_vector_allocator &
      operator=(const allocator_type &other) BOOST_NOEXCEPT_OR_NOTHROW
   {  return static_cast<small_vector_allocator&>(this->allocator_type::operator=(other));  }

   //!Allocates storage from the standard-conforming allocator
   inline pointer allocate(size_type count, const_void_pointer hint = const_void_pointer())
   {  return allocator_traits_type::allocate(this->as_base(), count, hint);  }

   //!Deallocates previously allocated memory.
   //!Never throws
   void deallocate(pointer ptr, size_type n) BOOST_NOEXCEPT_OR_NOTHROW
   {
      if(!this->is_internal_storage(ptr))
         allocator_traits_type::deallocate(this->as_base(), ptr, n);
   }

   //!Returns the maximum number of elements that could be allocated.
   //!Never throws
   inline size_type max_size() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return allocator_traits_type::max_size(this->as_base());   }

   small_vector_allocator select_on_container_copy_construction() const
   {  return small_vector_allocator(allocator_traits_type::select_on_container_copy_construction(this->as_base())); }

   bool storage_is_unpropagable(pointer p) const
   {  return this->is_internal_storage(p) || allocator_traits_type::storage_is_unpropagable(this->as_base(), p);  }

   //!Swaps two allocators, does nothing
   //!because this small_vector_allocator is stateless
   inline friend void swap(small_vector_allocator &l, small_vector_allocator &r) BOOST_NOEXCEPT_OR_NOTHROW
   {  boost::adl_move_swap(l.as_base(), r.as_base());  }

   //!An small_vector_allocator always compares to true, as memory allocated with one
   //!instance can be deallocated by another instance (except for unpropagable storage)
   inline friend bool operator==(const small_vector_allocator &l, const small_vector_allocator &r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return allocator_traits_type::equal(l.as_base(), r.as_base());  }

   //!An small_vector_allocator always compares to false, as memory allocated with one
   //!instance can be deallocated by another instance
   inline friend bool operator!=(const small_vector_allocator &l, const small_vector_allocator &r) BOOST_NOEXCEPT_OR_NOTHROW
   {  return !(l == r);   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   public:

   typedef small_vector_base<value_type, allocator_type, Options>    derived_type;
   typedef typename dtl::vector_for_small_vector
      <value_type, allocator_type, Options>::type                    vector_type;

   inline bool is_internal_storage(const_pointer p) const
   {  return this->internal_storage() == p;  }

   public:
   inline const_pointer internal_storage() const BOOST_NOEXCEPT_OR_NOTHROW;
   inline pointer       internal_storage()       BOOST_NOEXCEPT_OR_NOTHROW;
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

template<class T, std::size_t N, std::size_t Alignment>
struct small_vector_storage
{
   typedef typename dtl::aligned_storage
      <sizeof(T)*N, Alignment>::type storage_type;
   storage_type m_storage;
   BOOST_STATIC_CONSTEXPR std::size_t sms_size = sizeof(storage_type)/sizeof(T);
};

template<class T, std::size_t Alignment>
struct small_vector_storage<T, 0u, Alignment>
{
   BOOST_STATIC_CONSTEXPR std::size_t sms_size = 0u;
};

//! This class consists of common code from all small_vector<T, N> types that don't depend on the
//! "N" template parameter. This class is non-copyable and non-destructible, so this class typically
//! used as reference argument to functions that read or write small vectors. Since `small_vector<T, N>`
//! derives from `small_vector_base<T>`, the conversion to `small_vector_base` is implicit
//! <pre>
//!
//! //Clients can pass any small_vector<Foo, N>.
//! void read_any_small_vector_of_foo(const small_vector_base<Foo> &in_parameter);
//!
//! void modify_any_small_vector_of_foo(small_vector_base<Foo> &in_out_parameter);
//!
//! void some_function()
//! {
//! 
//!    small_vector<Foo, 8> myvector;
//!
//!    read_any_small_vector_of_foo(myvector);   // Reads myvector
//!
//!    modify_any_small_vector_of_foo(myvector); // Modifies myvector
//! 
//! }
//! </pre>
//!
//! All `boost::container:vector` member functions are inherited. See `vector` documentation for details.
//!
template <class T, class SecAlloc, class Options>
class small_vector_base
#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   : public dtl::vector_for_small_vector<T, SecAlloc, Options>::type
#endif
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   public:
   //Make it public as it will be inherited by small_vector and container
   //must have this public member
   typedef typename real_allocator<T, SecAlloc>::type                     underlying_allocator_t;
   typedef typename allocator_traits<underlying_allocator_t>::
      template portable_rebind_alloc<void>::type                          void_underlying_allocator_t;
   typedef small_vector_allocator<T, void_underlying_allocator_t, Options>allocator_type;
   typedef typename dtl::get_small_vector_opt<Options>::type              options_t;
   typedef typename dtl::vector_for_small_vector
      <T, SecAlloc, Options>::type                                        base_type;
   typedef typename allocator_traits<allocator_type>::pointer             pointer;
   typedef typename allocator_traits<allocator_type>::const_pointer       const_pointer;
   typedef typename allocator_traits<allocator_type>::void_pointer        void_pointer;
   typedef typename allocator_traits<allocator_type>::const_void_pointer  const_void_pointer;
   typedef typename base_type::size_type                                  size_type;


   private: 
   BOOST_COPYABLE_AND_MOVABLE(small_vector_base)

   friend class small_vector_allocator<T, void_underlying_allocator_t, Options>;

   inline
   const_pointer internal_storage() const BOOST_NOEXCEPT_OR_NOTHROW
   {  return this->base_type::get_stored_allocator().internal_storage();   }

   inline
   pointer internal_storage() BOOST_NOEXCEPT_OR_NOTHROW
   {  return this->base_type::get_stored_allocator().internal_storage();   }

   private:
         base_type &as_base()       { return static_cast<base_type&>(*this); }
   const base_type &as_base() const { return static_cast<const base_type&>(*this); }

   public:

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD bool is_small() const
   {  return this->internal_storage() == this->data();   }

   protected:

   inline explicit small_vector_base(initial_capacity_t, size_type initial_capacity)
      : base_type(initial_capacity_t(), this->internal_storage(), initial_capacity)
   {}

   template<class AllocFwd>
   inline explicit small_vector_base(initial_capacity_t, size_type initial_capacity, BOOST_FWD_REF(AllocFwd) a)
      : base_type(initial_capacity_t(), this->internal_storage(), initial_capacity, ::boost::forward<AllocFwd>(a))
   {}

   template<class AllocFwd>
   inline explicit small_vector_base(initial_capacity_t, size_type initial_capacity, BOOST_FWD_REF(AllocFwd) a, small_vector_base &x)
      : base_type(initial_capacity_t(), this->internal_storage(), initial_capacity, ::boost::forward<AllocFwd>(a), x)
   {}

   inline explicit small_vector_base(maybe_initial_capacity_t, size_type initial_capacity, size_type initial_size)
      : base_type( maybe_initial_capacity_t()
                 , (initial_capacity >= initial_size) ? this->internal_storage() : pointer()
                 , (initial_capacity >= initial_size) ? initial_capacity : initial_size
                 )
   {}

   template<class AllocFwd>
   inline explicit small_vector_base(maybe_initial_capacity_t, size_type initial_capacity, size_type initial_size, BOOST_FWD_REF(AllocFwd) a)
      : base_type(maybe_initial_capacity_t()
                 , (initial_capacity >= initial_size) ? this->internal_storage() : pointer()
                 , (initial_capacity >= initial_size) ? initial_capacity : initial_size
                 , ::boost::forward<AllocFwd>(a)
      )
   {}

   void prot_shrink_to_fit_small(const size_type small_capacity)
   {  this->base_type::prot_shrink_to_fit_small(this->internal_storage(), small_capacity);  }

   using base_type::protected_set_size;

   //~small_vector_base(){}
   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   inline void prot_swap(small_vector_base& other, size_type internal_capacity_value)
   {  this->base_type::prot_swap_small(other, internal_capacity_value);  }

   public:
   inline small_vector_base& operator=(BOOST_COPY_ASSIGN_REF(small_vector_base) other)
   {  return static_cast<small_vector_base&>(this->base_type::operator=(static_cast<base_type const&>(other)));  }

   inline small_vector_base& operator=(BOOST_RV_REF(small_vector_base) other)
   {  return static_cast<small_vector_base&>(this->base_type::operator=(BOOST_MOVE_BASE(base_type, other))); }

   inline void swap(small_vector_base &other)
   {  return this->base_type::prot_swap_small(other, 0u);  }

};

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

/////////////////////////////////////////////////////
//
//          small_vector_storage_definer
//
/////////////////////////////////////////////////////
template<class T, std::size_t N, class Options>
struct small_vector_storage_definer
{
   typedef typename dtl::get_small_vector_opt<Options>::type options_t;
   BOOST_STATIC_CONSTEXPR std::size_t final_alignment =
      options_t::inplace_alignment ? options_t::inplace_alignment : dtl::alignment_of<T>::value;
   typedef small_vector_storage<T, N, final_alignment> type;
};


/// Figure out the offset of the first element. Idea taken from LLVM's SmallVector
template <class T, class SecAlloc, class Options>
struct small_vector_storage_offset
{
   typedef small_vector_base<T, SecAlloc, Options>                    base_type;
   typedef typename small_vector_storage_definer<T, 1, Options>::type storage_type;
   typename dtl::aligned_storage
      < sizeof(base_type), dtl::alignment_of<base_type>::value
      >::type base;

   typename dtl::aligned_storage
      < sizeof(storage_type), dtl::alignment_of<storage_type>::value
      > ::type storage;
};

template <class T, class SecAlloc, class Options>
inline std::size_t get_small_vector_storage_offset()
{
   typedef small_vector_storage_offset<T, SecAlloc, Options> struct_type;
   return offsetof(struct_type, storage);
}

#if defined(BOOST_GCC) && (BOOST_GCC >= 40600)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

//Internal storage hack
template<class T, class VoidAlloc, class Options>
inline typename small_vector_allocator<T, VoidAlloc, Options>::const_pointer
   small_vector_allocator<T, VoidAlloc, Options>::internal_storage() const BOOST_NOEXCEPT_OR_NOTHROW
{
   const vector_type& v = *static_cast<const vector_type*>(static_cast<const void *>(this));
   BOOST_ASSERT((std::size_t(this) % dtl::alignment_of< small_vector_storage_offset<T, allocator_type, Options> >::value) == 0);
   const char *addr = reinterpret_cast<const char*>(&v);
   typedef typename boost::intrusive::pointer_traits<pointer>::template rebind_pointer<const char>::type const_char_pointer;
   const_void_pointer vptr = boost::intrusive::pointer_traits<const_char_pointer>::pointer_to(*addr)
      + get_small_vector_storage_offset<T, allocator_type, Options>();
   return boost::intrusive::pointer_traits<const_pointer>::static_cast_from(vptr);
}

template <class T, class VoidAlloc, class Options>
inline typename small_vector_allocator<T, VoidAlloc, Options>::pointer
   small_vector_allocator<T, VoidAlloc, Options>::internal_storage() BOOST_NOEXCEPT_OR_NOTHROW
{
   vector_type& v = *static_cast<vector_type*>(static_cast<void*>(this));
   BOOST_ASSERT((std::size_t(this) % dtl::alignment_of< small_vector_storage_offset<T, allocator_type, Options> >::value) == 0);
   char* addr = reinterpret_cast<char*>(&v);
   typedef typename boost::intrusive::pointer_traits<pointer>::template rebind_pointer<char>::type char_pointer;
   void_pointer vptr = boost::intrusive::pointer_traits<char_pointer>::pointer_to(*addr)
                     + get_small_vector_storage_offset<T, allocator_type, Options>();
   return boost::intrusive::pointer_traits<pointer>::static_cast_from(vptr);
}

#if defined(BOOST_GCC) && (BOOST_GCC >= 40600)
#pragma GCC diagnostic pop
#endif

#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

//! small_vector is a vector-like container optimized for the case when it contains few elements.
//! It contains some preallocated elements in-place, which can avoid the use of dynamic storage allocation
//! when the actual number of elements is below that preallocated threshold.
//!
//! `small_vector<T, N, Allocator, Options>` is convertible to `small_vector_base<T, Allocator, Options>` that is independent
//! from the preallocated element capacity, so client code does not need to be templated on that N argument.
//!
//! All `boost::container::vector` member functions are inherited. See `vector` documentation for details.
//!
//! Any change to the capacity of the vector, including decreasing its size such as with the shrink_to_fit method, will
//! cause the vector to permanently switch to dynamically allocated storage.
//!
//! \tparam T The type of object that is stored in the small_vector
//! \tparam N The number of preallocated elements stored inside small_vector. It shall be less than Allocator::max_size();
//! \tparam Allocator The allocator used for memory management when the number of elements exceeds N. Use void
//!   for the default allocator
//! \tparam Options A type produced from \c boost::container::small_vector_options.
template <class T, std::size_t N, class Allocator BOOST_CONTAINER_DOCONLY(= void), class Options BOOST_CONTAINER_DOCONLY(= void) >
class small_vector
   : public small_vector_base<T, Allocator, Options>
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   , private small_vector_storage_definer<T, N, Options>::type
   #endif
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   BOOST_COPYABLE_AND_MOVABLE(small_vector)

   public:
   typedef small_vector_base<T, Allocator, Options>   base_type;
   typedef typename base_type::allocator_type         allocator_type;
   typedef typename base_type::size_type              size_type;
   typedef typename base_type::value_type             value_type;

   inline static size_type internal_capacity()
   {  return static_capacity;  }

   typedef allocator_traits<typename base_type::allocator_type> allocator_traits_type;

   #endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //! @brief The capacity/max size of the container
   BOOST_STATIC_CONSTEXPR size_type static_capacity = small_vector_storage_definer<T, N, Options>::type::sms_size;

   public:
   inline small_vector()
      BOOST_NOEXCEPT_IF(dtl::is_nothrow_default_constructible<allocator_type>::value)
      : base_type(initial_capacity_t(), internal_capacity())
   {}

   inline explicit small_vector(const allocator_type &a)
      : base_type(initial_capacity_t(), internal_capacity(), a)
   {}

   inline explicit small_vector(size_type n)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n)
   {  this->protected_init_n(n, value_init); }

   inline small_vector(size_type n, const allocator_type &a)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n, a)
   {  this->protected_init_n(n, value_init); }

   inline small_vector(size_type n, default_init_t)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n)
   {  this->protected_init_n(n, default_init_t()); }

   inline small_vector(size_type n, default_init_t, const allocator_type &a)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n, a)
   {  this->protected_init_n(n, default_init_t()); }

   inline small_vector(size_type n, const value_type &v)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n)
   {  this->protected_init_n(n, v); }

   inline small_vector(size_type n, const value_type &v, const allocator_type &a)
      : base_type(maybe_initial_capacity_t(), internal_capacity(), n, a)
   {  this->protected_init_n(n, v); }

   template <class InIt>
   inline small_vector(InIt first, InIt last
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_c
         < dtl::is_convertible<InIt BOOST_MOVE_I size_type>::value
         BOOST_MOVE_I dtl::nat >::type * = 0)
      )
      : base_type(initial_capacity_t(), internal_capacity())
   {  this->assign(first, last); }

   template <class InIt>
   inline small_vector(InIt first, InIt last, const allocator_type& a
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_c
         < dtl::is_convertible<InIt BOOST_MOVE_I size_type>::value
         BOOST_MOVE_I dtl::nat >::type * = 0)
      )
      : base_type(initial_capacity_t(), internal_capacity(), a)
   {  this->assign(first, last); }

   inline small_vector(const small_vector &other)
      : base_type( initial_capacity_t(), internal_capacity()
                 , allocator_traits_type::select_on_container_copy_construction(other.get_stored_allocator()))
   {  this->assign(other.cbegin(), other.cend());  }

   inline small_vector(const small_vector &other, const allocator_type &a)
      : base_type(initial_capacity_t(), internal_capacity(), a)
   {  this->assign(other.cbegin(), other.cend());  }

   inline explicit small_vector(const base_type &other)
      : base_type( initial_capacity_t(), internal_capacity()
                 , allocator_traits_type::select_on_container_copy_construction(other.get_stored_allocator()))
   {  this->assign(other.cbegin(), other.cend());  }

   inline explicit small_vector(BOOST_RV_REF(base_type) other)
      : base_type(initial_capacity_t(), internal_capacity(), ::boost::move(other.get_stored_allocator()), other)
   {}

   inline small_vector(BOOST_RV_REF(small_vector) other)
      BOOST_NOEXCEPT_IF(boost::container::dtl::is_nothrow_move_constructible<value_type>::value)
      : base_type(initial_capacity_t(), internal_capacity(), ::boost::move(other.get_stored_allocator()), other)
   {}

   inline small_vector(BOOST_RV_REF(small_vector) other, const allocator_type &a)
      : base_type(initial_capacity_t(), internal_capacity(), a, other)
   {}

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   inline small_vector(std::initializer_list<value_type> il, const allocator_type& a = allocator_type())
      : base_type(initial_capacity_t(), internal_capacity(), a)
   {
      this->assign(il.begin(), il.end());
   }
   #endif

   inline small_vector& operator=(BOOST_COPY_ASSIGN_REF(small_vector) other)
   {  return static_cast<small_vector&>(this->base_type::operator=(static_cast<base_type const&>(other)));  }

   inline small_vector& operator=(BOOST_RV_REF(small_vector) other)
      BOOST_NOEXCEPT_IF(boost::container::dtl::is_nothrow_move_assignable<value_type>::value
         && (allocator_traits_type::propagate_on_container_move_assignment::value
             || allocator_traits_type::is_always_equal::value))
   {  return static_cast<small_vector&>(this->base_type::operator=(BOOST_MOVE_BASE(base_type, other))); }

   inline small_vector& operator=(const base_type &other)
   {  return static_cast<small_vector&>(this->base_type::operator=(other));  }

   inline small_vector& operator=(BOOST_RV_REF(base_type) other)
   {  return static_cast<small_vector&>(this->base_type::operator=(boost::move(other))); }

   inline void swap(small_vector &other)
   {  return this->base_type::prot_swap(other, static_capacity);  }

   inline void shrink_to_fit()
   {  this->base_type::prot_shrink_to_fit_small(this->internal_capacity());   }
};

}}

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
/*
namespace boost {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator>
struct has_trivial_destructor_after_move<boost::container::vector<T, Allocator> >
{
   typedef typename ::boost::container::allocator_traits<Allocator>::pointer pointer;
   BOOST_STATIC_CONSTEXPR bool value = ::boost::has_trivial_destructor_after_move<Allocator>::value &&
                             ::boost::has_trivial_destructor_after_move<pointer>::value;
};

}
*/
#endif   //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

#include <boost/container/detail/config_end.hpp>

#endif //   #ifndef  BOOST_CONTAINER_CONTAINER_SMALL_VECTOR_HPP
