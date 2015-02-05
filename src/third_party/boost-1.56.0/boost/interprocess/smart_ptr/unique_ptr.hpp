//////////////////////////////////////////////////////////////////////////////
// I, Howard Hinnant, hereby place this code in the public domain.
//////////////////////////////////////////////////////////////////////////////
//
// This file is the adaptation for Interprocess of
// Howard Hinnant's unique_ptr emulation code.
//
// (C) Copyright Ion Gaztanaga 2006-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_UNIQUE_PTR_HPP_INCLUDED
#define BOOST_INTERPROCESS_UNIQUE_PTR_HPP_INCLUDED

#include <boost/interprocess/detail/config_begin.hpp>
#include <boost/interprocess/detail/workaround.hpp>
#include <boost/assert.hpp>
#include <boost/interprocess/detail/utilities.hpp>
#include <boost/interprocess/detail/pointer_type.hpp>
#include <boost/move/move.hpp>
#include <boost/compressed_pair.hpp>
#include <boost/static_assert.hpp>
#include <boost/interprocess/detail/mpl.hpp>
#include <boost/interprocess/detail/type_traits.hpp>
#include <boost/interprocess/smart_ptr/deleter.hpp>
#include <cstddef>

//!\file
//!Describes the smart pointer unique_ptr

namespace boost{
namespace interprocess{

/// @cond
template <class T, class D> class unique_ptr;

namespace ipcdetail {

template <class T> struct unique_ptr_error;

template <class T, class D>
struct unique_ptr_error<const unique_ptr<T, D> >
{
    typedef unique_ptr<T, D> type;
};

}  //namespace ipcdetail {
/// @endcond

//!Template unique_ptr stores a pointer to an object and deletes that object
//!using the associated deleter when it is itself destroyed (such as when
//!leaving block scope.
//!
//!The unique_ptr provides a semantics of strict ownership. A unique_ptr owns the
//!object it holds a pointer to.
//!
//!A unique_ptr is not CopyConstructible, nor CopyAssignable, however it is
//!MoveConstructible and Move-Assignable.
//!
//!The uses of unique_ptr include providing exception safety for dynamically
//!allocated memory, passing ownership of dynamically allocated memory to a
//!function, and returning dynamically allocated memory from a function
//!
//!A client-supplied template argument D must be a
//!function pointer or functor for which, given a value d of type D and a pointer
//!ptr to a type T*, the expression d(ptr) is
//!valid and has the effect of deallocating the pointer as appropriate for that
//!deleter. D may also be an lvalue-reference to a deleter.
//!
//!If the deleter D maintains state, it is intended that this state stay with
//!the associated pointer as ownership is transferred
//!from unique_ptr to unique_ptr. The deleter state need never be copied,
//!only moved or swapped as pointer ownership
//!is moved around. That is, the deleter need only be MoveConstructible,
//!MoveAssignable, and Swappable, and need not be CopyConstructible
//!(unless copied into the unique_ptr) nor CopyAssignable.
template <class T, class D>
class unique_ptr
{
   /// @cond
   struct nat  {int for_bool;};
   struct nat2 {int for_nullptr;};
   typedef int nat2::*nullptr_t;
   typedef typename ipcdetail::add_reference<D>::type deleter_reference;
   typedef typename ipcdetail::add_reference<const D>::type deleter_const_reference;
   /// @endcond

   public:

   typedef T element_type;
   typedef D deleter_type;
   typedef typename ipcdetail::pointer_type<T, D>::type pointer;

   //!Requires: D must be default constructible, and that construction must not
   //!throw an exception. D must not be a reference type.
   //!
   //!Effects: Constructs a unique_ptr which owns nothing.
   //!
   //!Postconditions: get() == 0. get_deleter() returns a reference to a
   //!default constructed deleter D.
   //!
   //!Throws: nothing.
   unique_ptr()
      :  ptr_(pointer(0))
   {}

   //!Requires: The expression D()(p) must be well formed. The default constructor
   //!of D must not throw an exception.
   //!
   //!D must not be a reference type.
   //!
   //!Effects: Constructs a unique_ptr which owns p.
   //!
   //!Postconditions: get() == p. get_deleter() returns a reference to a default constructed deleter D.
   //!
   //!Throws: nothing.
   explicit unique_ptr(pointer p)
      :  ptr_(p)
   {}

   //!Requires: The expression d(p) must be well formed.
   //!
   //!Postconditions: get() == p. get_deleter() returns a reference to the
   //!internally stored deleter. If D is a
   //!reference type then get_deleter() returns a reference to the lvalue d.
   //!
   //!Throws: nothing.
   unique_ptr(pointer p
             ,typename ipcdetail::if_<ipcdetail::is_reference<D>
                  ,D
                  ,typename ipcdetail::add_reference<const D>::type>::type d)
      : ptr_(p, d)
   {}

   //!Requires: If the deleter is not a reference type, construction of the
   //!deleter D from an lvalue D must not throw an exception.
   //!
   //!Effects: Constructs a unique_ptr which owns the pointer which u owns
   //!(if any). If the deleter is not a reference type, it is move constructed
   //!from u's deleter, otherwise the reference is copy constructed from u's deleter.
   //!
   //!After the construction, u no longer owns a pointer.
   //![ Note: The deleter constructor can be implemented with
   //!   boost::forward<D>. -end note ]
   //!
   //!Postconditions: get() == value u.get() had before the construction.
   //!get_deleter() returns a reference to the internally stored deleter which
   //!was constructed from u.get_deleter(). If D is a reference type then get_-
   //!deleter() and u.get_deleter() both reference the same lvalue deleter.
   //!
   //!Throws: nothing.
   unique_ptr(BOOST_RV_REF(unique_ptr) u)
      : ptr_(u.release(), boost::forward<D>(u.get_deleter()))
   {}

   //!Requires: If D is not a reference type, construction of the deleter
   //!D from an rvalue of type E must be well formed
   //!and not throw an exception. If D is a reference type, then E must be
   //!the same type as D (diagnostic required). unique_ptr<U, E>::pointer
   //!must be implicitly convertible to pointer.
   //!
   //!Effects: Constructs a unique_ptr which owns the pointer which u owns
   //!(if any). If the deleter is not a reference
   //!type, it is move constructed from u's deleter, otherwise the reference
   //!is copy constructed from u's deleter.
   //!
   //!After the construction, u no longer owns a pointer.
   //!
   //!postconditions get() == value u.get() had before the construction,
   //!modulo any required offset adjustments
   //!resulting from the cast from U* to T*. get_deleter() returns a reference to the internally stored deleter which
   //!was constructed from u.get_deleter().
   //!
   //!Throws: nothing.
   template <class U, class E>
   unique_ptr(BOOST_RV_REF_BEG unique_ptr<U, E> BOOST_RV_REF_END u,
      typename ipcdetail::enable_if_c<
            ipcdetail::is_convertible<typename unique_ptr<U, E>::pointer, pointer>::value &&
            ipcdetail::is_convertible<E, D>::value &&
            (
               !ipcdetail::is_reference<D>::value ||
               ipcdetail::is_same<D, E>::value
            )
            ,
            nat
            >::type = nat())
      : ptr_(const_cast<unique_ptr<U,E>&>(u).release(), boost::move<D>(u.get_deleter()))
   {}

   //!Effects: If get() == 0 there are no effects. Otherwise get_deleter()(get()).
   //!
   //!Throws: nothing.
   ~unique_ptr()
   {  reset(); }

   // assignment

   //!Requires: Assignment of the deleter D from an rvalue D must not throw an exception.
   //!
   //!Effects: reset(u.release()) followed by a move assignment from u's deleter to
   //!this deleter.
   //!
   //!Postconditions: This unique_ptr now owns the pointer which u owned, and u no
   //!longer owns it.
   //!
   //!Returns: *this.
   //!
   //!Throws: nothing.
   unique_ptr& operator=(BOOST_RV_REF(unique_ptr) u)
   {
      reset(u.release());
      ptr_.second() = boost::move(u.get_deleter());
      return *this;
   }

   //!Requires: Assignment of the deleter D from an rvalue D must not
   //!throw an exception. U* must be implicitly convertible to T*.
   //!
   //!Effects: reset(u.release()) followed by a move assignment from
   //!u's deleter to this deleter. If either D or E is
   //!a reference type, then the referenced lvalue deleter participates
   //!in the move assignment.
   //!
   //!Postconditions: This unique_ptr now owns the pointer which u owned,
   //!and u no longer owns it.
   //!
   //!Returns: *this.
   //!
   //!Throws: nothing.
   template <class U, class E>
   unique_ptr& operator=(BOOST_RV_REF_BEG unique_ptr<U, E> BOOST_RV_REF_END u)
   {
      reset(u.release());
      ptr_.second() = boost::move(u.get_deleter());
      return *this;
   }

   //!Assigns from the literal 0 or NULL.
   //!
   //!Effects: reset().
   //!
   //!Postcondition: get() == 0
   //!
   //!Returns: *this.
   //!
   //!Throws: nothing.
   unique_ptr& operator=(nullptr_t)
   {
      reset();
      return *this;
   }

   //!Requires: get() != 0.
   //!Returns: *get().
   //!Throws: nothing.
   typename ipcdetail::add_reference<T>::type operator*()  const
   {  return *ptr_.first();   }

   //!Requires: get() != 0.
   //!Returns: get().
   //!Throws: nothing.
   pointer operator->() const
   {  return ptr_.first(); }

   //!Returns: The stored pointer.
   //!Throws: nothing.
   pointer get()        const
   {  return ptr_.first(); }

   //!Returns: A reference to the stored deleter.
   //!
   //!Throws: nothing.
   deleter_reference       get_deleter()
   {  return ptr_.second();   }

   //!Returns: A const reference to the stored deleter.
   //!
   //!Throws: nothing.
   deleter_const_reference get_deleter() const
   {  return ptr_.second();   }

   //!Returns: An unspecified value that, when used in boolean
   //!contexts, is equivalent to get() != 0.
   //!
   //!Throws: nothing.
   operator int nat::*() const
   {  return ptr_.first() ? &nat::for_bool : 0;   }

   //!Postcondition: get() == 0.
   //!
   //!Returns: The value get() had at the start of the call to release.
   //!
   //!Throws: nothing.
   pointer release()
   {
      pointer tmp = ptr_.first();
      ptr_.first() = 0;
      return tmp;
   }

   //!Effects: If p == get() there are no effects. Otherwise get_deleter()(get()).
   //!
   //!Postconditions: get() == p.
   //!
   //!Throws: nothing.
   void reset(pointer p = 0)
   {
      if (ptr_.first() != p){
         if (ptr_.first())
            ptr_.second()(ptr_.first());
         ptr_.first() = p;
      }
   }

   //!Requires: The deleter D is Swappable and will not throw an exception under swap.
   //!
   //!Effects: The stored pointers of this and u are exchanged.
   //!   The stored deleters are swapped (unqualified).
   //!Throws: nothing.
   void swap(unique_ptr& u)
   {  ptr_.swap(u.ptr_);   }

   /// @cond
   private:
   boost::compressed_pair<pointer, D> ptr_;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(unique_ptr)
   template <class U, class E> unique_ptr(unique_ptr<U, E>&);
   template <class U> unique_ptr(U&, typename ipcdetail::unique_ptr_error<U>::type = 0);

   template <class U, class E> unique_ptr& operator=(unique_ptr<U, E>&);
   template <class U> typename ipcdetail::unique_ptr_error<U>::type operator=(U&);
   /// @endcond
};
/*
template <class T, class D>
class unique_ptr<T[], D>
{
    struct nat {int for_bool_;};
    typedef typename ipcdetail::add_reference<D>::type deleter_reference;
    typedef typename ipcdetail::add_reference<const D>::type deleter_const_reference;
public:
    typedef T element_type;
    typedef D deleter_type;
    typedef typename ipcdetail::pointer_type<T, D>::type pointer;

    // constructors
    unique_ptr() : ptr_(pointer()) {}
    explicit unique_ptr(pointer p) : ptr_(p) {}
    unique_ptr(pointer p, typename if_<
                          boost::is_reference<D>,
                          D,
                          typename ipcdetail::add_reference<const D>::type>::type d)
        : ptr_(p, d) {}
    unique_ptr(const unique_ptr& u)
        : ptr_(const_cast<unique_ptr&>(u).release(), u.get_deleter()) {}

    // destructor
    ~unique_ptr() {reset();}

    // assignment
    unique_ptr& operator=(const unique_ptr& cu)
    {
        unique_ptr& u = const_cast<unique_ptr&>(cu);
        reset(u.release());
        ptr_.second() = u.get_deleter();
        return *this;
    }
    unique_ptr& operator=(int nat::*)
    {
        reset();
        return *this;
    }

    // observers
    typename ipcdetail::add_reference<T>::type operator[](std::size_t i)  const {return ptr_.first()[i];}
    pointer get()        const {return ptr_.first();}
    deleter_reference       get_deleter()       {return ptr_.second();}
    deleter_const_reference get_deleter() const {return ptr_.second();}
    operator int nat::*() const {return ptr_.first() ? &nat::for_bool_ : 0;}

    // modifiers
    pointer release()
    {
        pointer tmp = ptr_.first();
        ptr_.first() = 0;
        return tmp;
    }
    void reset(pointer p = 0)
    {
        if (ptr_.first() != p)
        {
            if (ptr_.first())
                ptr_.second()(ptr_.first());
            ptr_.first() = p;
        }
    }
    void swap(unique_ptr& u) {ptr_.swap(u.ptr_);}
private:
    boost::compressed_pair<pointer, D> ptr_;

    template <class U, class E> unique_ptr(U p, E,
        typename boost::enable_if<boost::is_convertible<U, pointer> >::type* = 0);
    template <class U> explicit unique_ptr(U,
        typename boost::enable_if<boost::is_convertible<U, pointer> >::type* = 0);

    unique_ptr(unique_ptr&);
    template <class U> unique_ptr(U&, typename ipcdetail::unique_ptr_error<U>::type = 0);

    unique_ptr& operator=(unique_ptr&);
    template <class U> typename ipcdetail::unique_ptr_error<U>::type operator=(U&);
};

template <class T, class D, std::size_t N>
class unique_ptr<T[N], D>
{
    struct nat {int for_bool_;};
    typedef typename ipcdetail::add_reference<D>::type deleter_reference;
    typedef typename ipcdetail::add_reference<const D>::type deleter_const_reference;
public:
    typedef T element_type;
    typedef D deleter_type;
    typedef typename ipcdetail::pointer_type<T, D>::type pointer;
    static const std::size_t size = N;

    // constructors
    unique_ptr() : ptr_(0) {}
    explicit unique_ptr(pointer p) : ptr_(p) {}
    unique_ptr(pointer p, typename if_<
                         boost::is_reference<D>,
                         D,
                         typename ipcdetail::add_reference<const D>::type>::type d)
        : ptr_(p, d) {}
    unique_ptr(const unique_ptr& u)
        : ptr_(const_cast<unique_ptr&>(u).release(), u.get_deleter()) {}

    // destructor
    ~unique_ptr() {reset();}

    // assignment
    unique_ptr& operator=(const unique_ptr& cu)
    {
        unique_ptr& u = const_cast<unique_ptr&>(cu);
        reset(u.release());
        ptr_.second() = u.get_deleter();
        return *this;
    }
    unique_ptr& operator=(int nat::*)
    {
        reset();
        return *this;
    }

    // observers
    typename ipcdetail::add_reference<T>::type operator[](std::size_t i)  const {return ptr_.first()[i];}
    pointer get()        const {return ptr_.first();}
    deleter_reference       get_deleter()       {return ptr_.second();}
    deleter_const_reference get_deleter() const {return ptr_.second();}
    operator int nat::*() const {return ptr_.first() ? &nat::for_bool : 0;}

    // modifiers
    pointer release()
    {
        pointer tmp = ptr_.first();
        ptr_.first() = 0;
        return tmp;
    }
    void reset(pointer p = 0)
    {
        if (ptr_.first() != p)
        {
            if (ptr_.first())
                ptr_.second()(ptr_.first(), N);
            ptr_.first() = p;
        }
    }
    void swap(unique_ptr& u) {ptr_.swap(u.ptr_);}
private:
    boost::compressed_pair<pointer, D> ptr_;

    template <class U, class E> unique_ptr(U p, E,
        typename boost::enable_if<boost::is_convertible<U, pointer> >::type* = 0);
    template <class U> explicit unique_ptr(U,
        typename boost::enable_if<boost::is_convertible<U, pointer> >::type* = 0);

    unique_ptr(unique_ptr&);
    template <class U> unique_ptr(U&, typename ipcdetail::unique_ptr_error<U>::type = 0);

    unique_ptr& operator=(unique_ptr&);
    template <class U> typename ipcdetail::unique_ptr_error<U>::type operator=(U&);
};
*/
template <class T, class D> inline
void swap(unique_ptr<T, D>& x, unique_ptr<T, D>& y)
{  x.swap(y);  }

template <class T1, class D1, class T2, class D2> inline
bool operator==(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() == y.get(); }

template <class T1, class D1, class T2, class D2> inline
bool operator!=(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() != y.get(); }

template <class T1, class D1, class T2, class D2> inline
bool operator <(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() < y.get();  }

template <class T1, class D1, class T2, class D2> inline
bool operator<=(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() <= y.get(); }

template <class T1, class D1, class T2, class D2> inline
bool operator >(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() > y.get();  }

template <class T1, class D1, class T2, class D2> inline
bool operator>=(const unique_ptr<T1, D1>& x, const unique_ptr<T2, D2>& y)
{  return x.get() >= y.get(); }


//!Returns the type of a unique pointer
//!of type T with boost::interprocess::deleter deleter
//!that can be constructed in the given managed segment type.
template<class T, class ManagedMemory>
struct managed_unique_ptr
{
   typedef unique_ptr
   < T
   , typename ManagedMemory::template deleter<T>::type
   > type;
};

//!Returns an instance of a unique pointer constructed
//!with boost::interproces::deleter from a pointer
//!of type T that has been allocated in the passed managed segment
template<class T, class ManagedMemory>
inline typename managed_unique_ptr<T, ManagedMemory>::type
   make_managed_unique_ptr(T *constructed_object, ManagedMemory &managed_memory)
{
   return typename managed_unique_ptr<T, ManagedMemory>::type
      (constructed_object, managed_memory.template get_deleter<T>());
}

}  //namespace interprocess{
}  //namespace boost{

#include <boost/interprocess/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTERPROCESS_UNIQUE_PTR_HPP_INCLUDED
