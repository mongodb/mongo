//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_OFFSET_PTR_HPP
#define BOOST_INTERPROCESS_OFFSET_PTR_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>

#include <boost/interprocess/interprocess_fwd.hpp>
#include <boost/interprocess/detail/utilities.hpp>
#include <boost/interprocess/detail/cast_tags.hpp>
#include <boost/interprocess/detail/mpl.hpp>
#include <boost/assert.hpp>
#include <ostream>
#include <istream>
#include <iterator>
#include <boost/aligned_storage.hpp>
#include <boost/type_traits/alignment_of.hpp>

//!\file
//!Describes a smart pointer that stores the offset between this pointer and
//!target pointee, called offset_ptr.

namespace boost {

//Predeclarations
template <class T>
struct has_trivial_constructor;

template <class T>
struct has_trivial_destructor;

namespace interprocess {

/// @cond
namespace ipcdetail {

   template<class OffsetType, std::size_t OffsetAlignment>
   union offset_ptr_internal
   {
      explicit offset_ptr_internal(OffsetType off)
         : m_offset(off)
      {}
      OffsetType m_offset; //Distance between this object and pointee address
      typename ::boost::aligned_storage
         < sizeof(OffsetType)
         , (OffsetAlignment == offset_type_alignment) ?
            ::boost::alignment_of<OffsetType>::value : OffsetAlignment
         >::type alignment_helper;
   };

   //Note: using the address of a local variable to point to another address
   //is not standard conforming and this can be optimized-away by the compiler.
   //Non-inlining is a method to remain illegal but correct

   //Undef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_XXX if your compiler can inline
   //this code without breaking the library

   ////////////////////////////////////////////////////////////////////////
   //
   //                      offset_ptr_to_raw_pointer
   //
   ////////////////////////////////////////////////////////////////////////
   #define BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_PTR
   #define BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_PTR

   template<int Dummy>
   #ifndef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_PTR
      BOOST_INTERPROCESS_NEVER_INLINE
   #elif defined(NDEBUG)
      inline
   #endif
   void * offset_ptr_to_raw_pointer(const volatile void *this_ptr, std::size_t offset)
   {
      typedef pointer_size_t_caster<void*> caster_t;
      #ifndef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_PTR
         if(offset == 1){
            return 0;
         }
         else{
            const caster_t caster((void*)this_ptr);
            return caster_t(caster.size() + offset).pointer();
         }
      #else
         const caster_t caster((void*)this_ptr);
         return caster_t((caster.size() + offset) & -std::size_t(offset != 1)).pointer();
      #endif
   }

   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_PTR
      #undef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_PTR
   #endif
   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_PTR
      #undef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_PTR
   #endif

   ////////////////////////////////////////////////////////////////////////
   //
   //                      offset_ptr_to_offset
   //
   ////////////////////////////////////////////////////////////////////////
   #define BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF
   //Branchless seems slower in x86
   #define BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF

   template<int Dummy>
   #ifndef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF
      BOOST_INTERPROCESS_NEVER_INLINE
   #elif defined(NDEBUG)
      inline
   #endif
   std::size_t offset_ptr_to_offset(const volatile void *ptr, const volatile void *this_ptr)
   {
      typedef pointer_size_t_caster<void*> caster_t;
      #ifndef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF
         //offset == 1 && ptr != 0 is not legal for this pointer
         if(!ptr){
            return 1;
         }
         else{
            const caster_t this_caster((void*)this_ptr);
            const caster_t ptr_caster((void*)ptr);
            std::size_t offset = ptr_caster.size() - this_caster.size();
            BOOST_ASSERT(offset != 1);
            return offset;
         }
      #else
         const caster_t this_caster((void*)this_ptr);
         const caster_t ptr_caster((void*)ptr);
         //std::size_t other = -std::size_t(ptr != 0);
         //std::size_t offset = (ptr_caster.size() - this_caster.size()) & other;
         //return offset + !other;
         //
         std::size_t offset = (ptr_caster.size() - this_caster.size() - 1) & -std::size_t(ptr != 0);
         return ++offset;
      #endif
   }

   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF
      #undef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF
   #endif
   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF
      #undef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF
   #endif

   ////////////////////////////////////////////////////////////////////////
   //
   //                      offset_ptr_to_offset_from_other
   //
   ////////////////////////////////////////////////////////////////////////
   #define BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF_FROM_OTHER
   //Branchless seems slower in x86
   #define BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF_FROM_OTHER

   template<int Dummy>
   #ifndef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF_FROM_OTHER
      BOOST_INTERPROCESS_NEVER_INLINE
   #elif defined(NDEBUG)
      inline
   #endif
   std::size_t offset_ptr_to_offset_from_other
      (const volatile void *this_ptr, const volatile void *other_ptr, std::size_t other_offset)
   {
      typedef pointer_size_t_caster<void*> caster_t;
      #ifndef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF_FROM_OTHER
      if(other_offset == 1){
         return 1;
      }
      else{
         const caster_t this_caster((void*)this_ptr);
         const caster_t other_caster((void*)other_ptr);
         std::size_t offset = other_caster.size() - this_caster.size() + other_offset;
         BOOST_ASSERT(offset != 1);
         return offset;
      }
      #else
      const caster_t this_caster((void*)this_ptr);
      const caster_t other_caster((void*)other_ptr);
      return ((other_caster.size() - this_caster.size()) & -std::size_t(other_offset != 1)) + other_offset;
      #endif
   }

   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF_FROM_OTHER
      #undef BOOST_INTERPROCESS_OFFSET_PTR_INLINE_TO_OFF_FROM_OTHER
   #endif
   #ifdef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF_FROM_OTHER
      #undef BOOST_INTERPROCESS_OFFSET_PTR_BRANCHLESS_TO_OFF_FROM_OTHER
   #endif

   ////////////////////////////////////////////////////////////////////////
   //
   // Let's assume cast to void and cv cast don't change any target address
   //
   ////////////////////////////////////////////////////////////////////////
   template<class From, class To>
   struct offset_ptr_maintains_address
   {
      static const bool value =    ipcdetail::is_cv_same<From, To>::value
                                || ipcdetail::is_cv_same<void, To>::value;
   };

}  //namespace ipcdetail {
/// @endcond

//!A smart pointer that stores the offset between between the pointer and the
//!the object it points. This allows offset allows special properties, since
//!the pointer is independent from the address address of the pointee, if the
//!pointer and the pointee are still separated by the same offset. This feature
//!converts offset_ptr in a smart pointer that can be placed in shared memory and
//!memory mapped files mapped in different addresses in every process.
template <class PointedType, class DifferenceType, class OffsetType, std::size_t OffsetAlignment>
class offset_ptr
{
   /// @cond
   typedef offset_ptr<PointedType, DifferenceType, OffsetType, OffsetAlignment>   self_t;
   void unspecified_bool_type_func() const {}
   typedef void (self_t::*unspecified_bool_type)() const;
   /// @endcond

   public:
   typedef PointedType                       element_type;
   typedef PointedType *                     pointer;
   typedef typename ipcdetail::
      add_reference<PointedType>::type       reference;
   typedef typename ipcdetail::
      remove_volatile<typename ipcdetail::
         remove_const<PointedType>::type
            >::type                          value_type;
   typedef DifferenceType                    difference_type;
   typedef std::random_access_iterator_tag   iterator_category;
   typedef OffsetType                        offset_type;

   public:   //Public Functions

   //!Default constructor (null pointer).
   //!Never throws.
   offset_ptr()
      : internal(1)
   {}

   //!Constructor from raw pointer (allows "0" pointer conversion).
   //!Never throws.
   offset_ptr(pointer ptr)
      : internal(static_cast<OffsetType>(ipcdetail::offset_ptr_to_offset<0>(ptr, this)))
   {}

   //!Constructor from other pointer.
   //!Never throws.
   template <class T>
   offset_ptr( T *ptr
             , typename ipcdetail::enable_if< ipcdetail::is_convertible<T*, PointedType*> >::type * = 0)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset<0>(static_cast<PointedType*>(ptr), this)))
   {}

   //!Constructor from other offset_ptr
   //!Never throws.
   offset_ptr(const offset_ptr& ptr)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset_from_other<0>(this, &ptr, ptr.internal.m_offset)))
   {}

   //!Constructor from other offset_ptr. If pointers of pointee types are
   //!convertible, offset_ptrs will be convertibles. Never throws.
   template<class T2>
   offset_ptr( const offset_ptr<T2, DifferenceType, OffsetType, OffsetAlignment> &ptr
             #ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED
             , typename ipcdetail::enable_if_c< ipcdetail::is_convertible<T2*, PointedType*>::value 
               && ipcdetail::offset_ptr_maintains_address<T2, PointedType>::value
             >::type * = 0
             #endif
             )
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset_from_other<0>(this, &ptr, ptr.get_offset())))
   {}

   #ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED

   //!Constructor from other offset_ptr. If pointers of pointee types are
   //!convertible, offset_ptrs will be convertibles. Never throws.
   template<class T2>
   offset_ptr( const offset_ptr<T2, DifferenceType, OffsetType, OffsetAlignment> &ptr
             , typename ipcdetail::enable_if_c< ipcdetail::is_convertible<T2*, PointedType*>::value
               && !ipcdetail::offset_ptr_maintains_address<T2, PointedType>::value
             >::type * = 0)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset<0>(static_cast<PointedType*>(ptr.get()), this)))
   {}

   #endif

   //!Emulates static_cast operator.
   //!Never throws.
   template<class T2, class P2, class O2, std::size_t A2>
   offset_ptr(const offset_ptr<T2, P2, O2, A2> & r, ipcdetail::static_cast_tag)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset<0>(static_cast<PointedType*>(r.get()), this)))
   {}

   //!Emulates const_cast operator.
   //!Never throws.
   template<class T2, class P2, class O2, std::size_t A2>
   offset_ptr(const offset_ptr<T2, P2, O2, A2> & r, ipcdetail::const_cast_tag)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset<0>(const_cast<PointedType*>(r.get()), this)))
   {}

   //!Emulates dynamic_cast operator.
   //!Never throws.
   template<class T2, class P2, class O2, std::size_t A2>
   offset_ptr(const offset_ptr<T2, P2, O2, A2> & r, ipcdetail::dynamic_cast_tag)
      : internal(static_cast<OffsetType>
         (ipcdetail::offset_ptr_to_offset<0>(dynamic_cast<PointedType*>(r.get()), this)))
   {}

   //!Emulates reinterpret_cast operator.
   //!Never throws.
   template<class T2, class P2, class O2, std::size_t A2>
   offset_ptr(const offset_ptr<T2, P2, O2, A2> & r, ipcdetail::reinterpret_cast_tag)
      : internal(static_cast<OffsetType>
      (ipcdetail::offset_ptr_to_offset<0>(reinterpret_cast<PointedType*>(r.get()), this)))
   {}

   //!Obtains raw pointer from offset.
   //!Never throws.
   pointer get() const
   {  return (pointer)ipcdetail::offset_ptr_to_raw_pointer<0>(this, this->internal.m_offset);   }

   offset_type get_offset() const
   {  return this->internal.m_offset;  }

   //!Pointer-like -> operator. It can return 0 pointer.
   //!Never throws.
   pointer operator->() const
   {  return this->get(); }

   //!Dereferencing operator, if it is a null offset_ptr behavior
   //!   is undefined. Never throws.
   reference operator* () const
   {
      pointer p = this->get();
      reference r = *p;
      return r;
   }

   //!Indexing operator.
   //!Never throws.
   reference operator[](difference_type idx) const
   {  return this->get()[idx];  }

   //!Assignment from pointer (saves extra conversion).
   //!Never throws.
   offset_ptr& operator= (pointer from)
   {
      this->internal.m_offset =
         static_cast<OffsetType>(ipcdetail::offset_ptr_to_offset<0>(from, this));
      return *this;
   }

   //!Assignment from other offset_ptr.
   //!Never throws.
   offset_ptr& operator= (const offset_ptr & ptr)
   {
      this->internal.m_offset =
         static_cast<OffsetType>(ipcdetail::offset_ptr_to_offset_from_other<0>(this, &ptr, ptr.internal.m_offset));
      return *this;
   }

   //!Assignment from related offset_ptr. If pointers of pointee types
   //!   are assignable, offset_ptrs will be assignable. Never throws.
   template<class T2>
   #ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED
   typename ipcdetail::enable_if_c< ipcdetail::is_convertible<T2*, PointedType*>::value
                                    && ipcdetail::offset_ptr_maintains_address<T2, PointedType>::value
                                  , offset_ptr&>::type
   #else
   offset_ptr&
   #endif
      operator= (const offset_ptr<T2, DifferenceType, OffsetType, OffsetAlignment> &ptr)
   {
      this->internal.m_offset =
         static_cast<OffsetType>(ipcdetail::offset_ptr_to_offset_from_other<0>(this, &ptr, ptr.get_offset()));
      return *this;
   }

   #ifndef BOOST_INTERPROCESS_DOXYGEN_INVOKED
   template<class T2>
   typename ipcdetail::enable_if_c<ipcdetail::is_convertible<T2*, PointedType*>::value 
                                   && !ipcdetail::offset_ptr_maintains_address<T2, PointedType>::value
                                 , offset_ptr&>::type
      operator= (const offset_ptr<T2, DifferenceType, OffsetType, OffsetAlignment> &ptr)
   {
      this->internal.m_offset =
         static_cast<OffsetType>(ipcdetail::offset_ptr_to_offset<0>(static_cast<PointedType*>(ptr.get()), this));
      return *this;
   }
   #endif

   //!offset_ptr += difference_type.
   //!Never throws.
   offset_ptr &operator+= (difference_type offset)
   {  this->inc_offset(offset * sizeof (PointedType));   return *this;  }

   //!offset_ptr -= difference_type.
   //!Never throws.
   offset_ptr &operator-= (difference_type offset)
   {  this->dec_offset(offset * sizeof (PointedType));   return *this;  }

   //!++offset_ptr.
   //!Never throws.
   offset_ptr& operator++ (void)
   {  this->inc_offset(sizeof (PointedType));   return *this;  }

   //!offset_ptr++.
   //!Never throws.
   offset_ptr operator++ (int)
   {
      offset_ptr tmp(*this);
      this->inc_offset(sizeof (PointedType));
      return tmp;
   }

   //!--offset_ptr.
   //!Never throws.
   offset_ptr& operator-- (void)
   {  this->dec_offset(sizeof (PointedType));   return *this;  }

   //!offset_ptr--.
   //!Never throws.
   offset_ptr operator-- (int)
   {
      offset_ptr tmp(*this);
      this->dec_offset(sizeof (PointedType));
      return tmp;
   }

   //!safe bool conversion operator.
   //!Never throws.
   operator unspecified_bool_type() const
   {  return this->internal.m_offset != 1? &self_t::unspecified_bool_type_func : 0;   }

   //!Not operator. Not needed in theory, but improves portability.
   //!Never throws
   bool operator! () const
   {  return this->internal.m_offset == 1;   }

   //!Compatibility with pointer_traits
   //!
   template <class U>
   struct rebind
   {  typedef offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment> other;  };

   //!Compatibility with pointer_traits
   //!
   static offset_ptr pointer_to(reference r)
   { return offset_ptr(&r); }

   //!difference_type + offset_ptr
   //!operation
   friend offset_ptr operator+(difference_type diff, offset_ptr right)
   {  right += diff;  return right;  }

   //!offset_ptr + difference_type
   //!operation
   friend offset_ptr operator+(offset_ptr left, difference_type diff)
   {  left += diff;  return left; }

   //!offset_ptr - diff
   //!operation
   friend offset_ptr operator-(offset_ptr left, difference_type diff)
   {  left -= diff;  return left; }

   //!offset_ptr - diff
   //!operation
   friend offset_ptr operator-(difference_type diff, offset_ptr right)
   {  right -= diff; return right; }

   //!offset_ptr - offset_ptr
   //!operation
   friend difference_type operator-(const offset_ptr &pt, const offset_ptr &pt2)
   {  return difference_type(pt.get()- pt2.get());   }

   //Comparison
   friend bool operator== (const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() == pt2.get();  }

   friend bool operator!= (const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() != pt2.get();  }

   friend bool operator<(const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() < pt2.get();  }

   friend bool operator<=(const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() <= pt2.get();  }

   friend bool operator>(const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() > pt2.get();  }

   friend bool operator>=(const offset_ptr &pt1, const offset_ptr &pt2)
   {  return pt1.get() >= pt2.get();  }

   //Comparison to raw ptr to support literal 0
   friend bool operator== (pointer pt1, const offset_ptr &pt2)
   {  return pt1 == pt2.get();  }

   friend bool operator!= (pointer pt1, const offset_ptr &pt2)
   {  return pt1 != pt2.get();  }

   friend bool operator<(pointer pt1, const offset_ptr &pt2)
   {  return pt1 < pt2.get();  }

   friend bool operator<=(pointer pt1, const offset_ptr &pt2)
   {  return pt1 <= pt2.get();  }

   friend bool operator>(pointer pt1, const offset_ptr &pt2)
   {  return pt1 > pt2.get();  }

   friend bool operator>=(pointer pt1, const offset_ptr &pt2)
   {  return pt1 >= pt2.get();  }

   //Comparison
   friend bool operator== (const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() == pt2;  }

   friend bool operator!= (const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() != pt2;  }

   friend bool operator<(const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() < pt2;  }

   friend bool operator<=(const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() <= pt2;  }

   friend bool operator>(const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() > pt2;  }

   friend bool operator>=(const offset_ptr &pt1, pointer pt2)
   {  return pt1.get() >= pt2;  }

   friend void swap(offset_ptr &left, offset_ptr &right)
   {
      pointer ptr = right.get();
      right = left;
      left = ptr;
   }

   private:
   /// @cond
   void inc_offset(DifferenceType bytes)
   {  internal.m_offset += bytes;   }

   void dec_offset(DifferenceType bytes)
   {  internal.m_offset -= bytes;   }

   ipcdetail::offset_ptr_internal<OffsetType, OffsetAlignment> internal;
   /// @endcond
};

//!operator<<
//!for offset ptr
template<class E, class T, class W, class X, class Y, std::size_t Z>
inline std::basic_ostream<E, T> & operator<<
   (std::basic_ostream<E, T> & os, offset_ptr<W, X, Y, Z> const & p)
{  return os << p.get_offset();   }

//!operator>>
//!for offset ptr
template<class E, class T, class W, class X, class Y, std::size_t Z>
inline std::basic_istream<E, T> & operator>>
   (std::basic_istream<E, T> & is, offset_ptr<W, X, Y, Z> & p)
{  return is >> p.get_offset();  }

//!Simulation of static_cast between pointers. Never throws.
template<class T1, class P1, class O1, std::size_t A1, class T2, class P2, class O2, std::size_t A2>
inline boost::interprocess::offset_ptr<T1, P1, O1, A1>
   static_pointer_cast(const boost::interprocess::offset_ptr<T2, P2, O2, A2> & r)
{
   return boost::interprocess::offset_ptr<T1, P1, O1, A1>
            (r, boost::interprocess::ipcdetail::static_cast_tag());
}

//!Simulation of const_cast between pointers. Never throws.
template<class T1, class P1, class O1, std::size_t A1, class T2, class P2, class O2, std::size_t A2>
inline boost::interprocess::offset_ptr<T1, P1, O1, A1>
   const_pointer_cast(const boost::interprocess::offset_ptr<T2, P2, O2, A2> & r)
{
   return boost::interprocess::offset_ptr<T1, P1, O1, A1>
            (r, boost::interprocess::ipcdetail::const_cast_tag());
}

//!Simulation of dynamic_cast between pointers. Never throws.
template<class T1, class P1, class O1, std::size_t A1, class T2, class P2, class O2, std::size_t A2>
inline boost::interprocess::offset_ptr<T1, P1, O1, A1>
   dynamic_pointer_cast(const boost::interprocess::offset_ptr<T2, P2, O2, A2> & r)
{
   return boost::interprocess::offset_ptr<T1, P1, O1, A1>
            (r, boost::interprocess::ipcdetail::dynamic_cast_tag());
}

//!Simulation of reinterpret_cast between pointers. Never throws.
template<class T1, class P1, class O1, std::size_t A1, class T2, class P2, class O2, std::size_t A2>
inline boost::interprocess::offset_ptr<T1, P1, O1, A1>
   reinterpret_pointer_cast(const boost::interprocess::offset_ptr<T2, P2, O2, A2> & r)
{
   return boost::interprocess::offset_ptr<T1, P1, O1, A1>
            (r, boost::interprocess::ipcdetail::reinterpret_cast_tag());
}

}  //namespace interprocess {

/// @cond

//!has_trivial_constructor<> == true_type specialization for optimizations
template <class T, class P, class O, std::size_t A>
struct has_trivial_constructor< boost::interprocess::offset_ptr<T, P, O, A> >
{
   static const bool value = true;
};

///has_trivial_destructor<> == true_type specialization for optimizations
template <class T, class P, class O, std::size_t A>
struct has_trivial_destructor< boost::interprocess::offset_ptr<T, P, O, A> >
{
   static const bool value = true;
};


namespace interprocess {

//!to_raw_pointer() enables boost::mem_fn to recognize offset_ptr.
//!Never throws.
template <class T, class P, class O, std::size_t A>
inline T * to_raw_pointer(boost::interprocess::offset_ptr<T, P, O, A> const & p)
{  return ipcdetail::to_raw_pointer(p);   }

}  //namespace interprocess


/// @endcond
}  //namespace boost {

/// @cond

namespace boost{

//This is to support embedding a bit in the pointer
//for intrusive containers, saving space
namespace intrusive {

//Predeclaration to avoid including header
template<class VoidPointer, std::size_t N>
struct max_pointer_plus_bits;

template<std::size_t OffsetAlignment, class P, class O, std::size_t A>
struct max_pointer_plus_bits<boost::interprocess::offset_ptr<void, P, O, A>, OffsetAlignment>
{
   //The offset ptr can embed one bit less than the alignment since it
   //uses offset == 1 to store the null pointer.
   static const std::size_t value = ::boost::interprocess::ipcdetail::ls_zeros<OffsetAlignment>::value - 1;
};

//Predeclaration
template<class Pointer, std::size_t NumBits>
struct pointer_plus_bits;

template<class T, class P, class O, std::size_t A, std::size_t NumBits>
struct pointer_plus_bits<boost::interprocess::offset_ptr<T, P, O, A>, NumBits>
{
   typedef boost::interprocess::offset_ptr<T, P, O, A>      pointer;
   typedef ::boost::interprocess::pointer_size_t_caster<T*> caster_t;
   //Bits are stored in the lower bits of the pointer except the LSB,
   //because this bit is used to represent the null pointer.
   static const std::size_t Mask = ((std::size_t(1) << NumBits) - 1) << 1u;

   static pointer get_pointer(const pointer &n)
   {
      caster_t caster(n.get());
      return pointer(caster_t(caster.size() & ~Mask).pointer());
   }

   static void set_pointer(pointer &n, const pointer &p)
   {
      caster_t n_caster(n.get());
      caster_t p_caster(p.get());
      BOOST_ASSERT(0 == (p_caster.size() & Mask));
      n = caster_t(p_caster.size() | (n_caster.size() & Mask)).pointer();
   }

   static std::size_t get_bits(const pointer &n)
   {  return (caster_t(n.get()).size() & Mask) >> 1u;  }

   static void set_bits(pointer &n, std::size_t b)
   {
      BOOST_ASSERT(b < (std::size_t(1) << NumBits));
      caster_t n_caster(n.get());
      n = caster_t((n_caster.size() & ~Mask) | (b << 1u)).pointer();
   }
};

}  //namespace intrusive

//Predeclaration
template<class T, class U>
struct pointer_to_other;

//Backwards compatibility with pointer_to_other
template <class PointedType, class DifferenceType, class OffsetType, std::size_t OffsetAlignment, class U>
struct pointer_to_other
   < ::boost::interprocess::offset_ptr<PointedType, DifferenceType, OffsetType, OffsetAlignment>, U >
{
   typedef ::boost::interprocess::offset_ptr<U, DifferenceType, OffsetType, OffsetAlignment> type;
};

}  //namespace boost{
/// @endcond

#include <boost/interprocess/detail/config_end.hpp>

#endif //#ifndef BOOST_INTERPROCESS_OFFSET_PTR_HPP
