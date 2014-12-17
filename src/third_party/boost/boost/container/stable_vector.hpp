//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
// Stable vector.
//
// Copyright 2008 Joaquin M Lopez Munoz.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_STABLE_VECTOR_HPP
#define BOOST_CONTAINER_STABLE_VECTOR_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/not.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/container/detail/version_type.hpp>
#include <boost/container/detail/multiallocation_chain.hpp>
#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/algorithms.hpp>
#include <boost/container/allocator/allocator_traits.hpp>
#include <boost/intrusive/pointer_traits.hpp>

#include <algorithm>
#include <stdexcept>
#include <memory>

///@cond

#include <boost/container/vector.hpp>

//#define STABLE_VECTOR_ENABLE_INVARIANT_CHECKING

#if defined(STABLE_VECTOR_ENABLE_INVARIANT_CHECKING)
#include <boost/assert.hpp>
#endif

///@endcond

namespace boost {
namespace container {

///@cond

namespace stable_vector_detail{

template<class SmartPtr>
struct smart_ptr_type
{
   typedef typename SmartPtr::value_type value_type;
   typedef value_type *pointer;
   static pointer get (const SmartPtr &smartptr)
   {  return smartptr.get();}
};

template<class T>
struct smart_ptr_type<T*>
{
   typedef T value_type;
   typedef value_type *pointer;
   static pointer get (pointer ptr)
   {  return ptr;}
};

template<class Ptr>
inline typename smart_ptr_type<Ptr>::pointer to_raw_pointer(const Ptr &ptr)
{  return smart_ptr_type<Ptr>::get(ptr);   }

template <class C>
class clear_on_destroy
{
   public:
   clear_on_destroy(C &c)
      :  c_(c), do_clear_(true)
   {}

   void release()
   {  do_clear_ = false; }

   ~clear_on_destroy()
   {
      if(do_clear_){
         c_.clear();
         c_.clear_pool();  
      }
   }

   private:
   clear_on_destroy(const clear_on_destroy &);
   clear_on_destroy &operator=(const clear_on_destroy &);
   C &c_;
   bool do_clear_;
};

template<class VoidPtr>
struct node_type_base
{/*
   node_type_base(VoidPtr p)
      : up(p)
   {}*/
   node_type_base()
   {}
   void set_pointer(VoidPtr p)
   {  up = p; }

   VoidPtr up;
};

template<typename VoidPointer, typename T>
struct node_type
   : public node_type_base<VoidPointer>
{
   node_type()
      : value()
   {}

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   template<class ...Args>
   node_type(Args &&...args)
      : value(boost::forward<Args>(args)...)
   {}

   #else //BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                           \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)    \
   node_type(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                             \
      : value(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))                         \
   {}                                                                                        \
   //!
   #define BOOST_PP_LOCAL_LIMITS (1, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif//BOOST_CONTAINER_PERFECT_FORWARDING
   
   void set_pointer(VoidPointer p)
   {  node_type_base<VoidPointer>::set_pointer(p); }

   T value;
};

template<typename T, typename Reference, typename Pointer>
class iterator
   : public std::iterator< std::random_access_iterator_tag
                         , T
                         , typename boost::intrusive::
                              pointer_traits<Pointer>::difference_type
                         , Pointer
                         , Reference>
{
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::template
         rebind_pointer<void>::type                void_ptr;
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::template
         rebind_pointer<const void>::type          const_void_ptr;
   typedef node_type<void_ptr, T>                  node_type_t;
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::template
         rebind_pointer<node_type_t>::type         node_type_ptr_t;
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::template
         rebind_pointer<const node_type_t>::type   const_node_type_ptr_t;
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::template
         rebind_pointer<void_ptr>::type            void_ptr_ptr;

   friend class iterator<T, const T, typename boost::intrusive::pointer_traits<Pointer>::template rebind_pointer<T>::type>;

   public:
   typedef std::random_access_iterator_tag      iterator_category;
   typedef T                                    value_type;
   typedef typename boost::intrusive::
      pointer_traits<Pointer>::difference_type  difference_type;
   typedef Pointer                              pointer;
   typedef Reference                            reference;

   iterator()
   {}

   explicit iterator(node_type_ptr_t pn)
      : pn(pn)
   {}

   iterator(const iterator<T, T&, typename boost::intrusive::pointer_traits<Pointer>::template rebind_pointer<T>::type>& x)
      : pn(x.pn)
   {}
   
   private:
   static node_type_ptr_t node_ptr_cast(const void_ptr &p)
   {
      return node_type_ptr_t(static_cast<node_type_t*>(stable_vector_detail::to_raw_pointer(p)));
   }

   static const_node_type_ptr_t node_ptr_cast(const const_void_ptr &p)
   {
      return const_node_type_ptr_t(static_cast<const node_type_t*>(stable_vector_detail::to_raw_pointer(p)));
   }

   static void_ptr_ptr void_ptr_ptr_cast(const void_ptr &p)
   {
      return void_ptr_ptr(static_cast<void_ptr*>(stable_vector_detail::to_raw_pointer(p)));
   }

   reference dereference() const
   {  return pn->value; }
   bool equal(const iterator& x) const
   {  return pn==x.pn;  }
   void increment()
   {  pn = node_ptr_cast(*(void_ptr_ptr_cast(pn->up)+1)); }
   void decrement()
   {  pn = node_ptr_cast(*(void_ptr_ptr_cast(pn->up)-1)); }
   void advance(difference_type n)
   {  pn = node_ptr_cast(*(void_ptr_ptr_cast(pn->up)+n)); }
   difference_type distance_to(const iterator& x)const
   {  return void_ptr_ptr_cast(x.pn->up) - void_ptr_ptr_cast(pn->up);   }

   public:
   //Pointer like operators
   reference operator*()  const {  return  this->dereference();  }
   pointer   operator->() const {  return  pointer(&this->dereference());  }

   //Increment / Decrement
   iterator& operator++()  
   {  this->increment(); return *this;  }

   iterator operator++(int)
   {  iterator tmp(*this);  ++*this; return iterator(tmp); }

   iterator& operator--()
   {  this->decrement(); return *this;  }

   iterator operator--(int)
   {  iterator tmp(*this);  --*this; return iterator(tmp);  }

   reference operator[](difference_type off) const
   {
      iterator tmp(*this);
      tmp += off;
      return *tmp;
   }

   iterator& operator+=(difference_type off)
   {
      pn = node_ptr_cast(*(void_ptr_ptr_cast(pn->up)+off));
      return *this;
   }

   friend iterator operator+(const iterator &left, difference_type off)
   {
      iterator tmp(left);
      tmp += off;
      return tmp;
   }

   friend iterator operator+(difference_type off, const iterator& right)
   {
      iterator tmp(right);
      tmp += off;
      return tmp;
   }

   iterator& operator-=(difference_type off)
   {  *this += -off; return *this;   }

   friend iterator operator-(const iterator &left, difference_type off)
   {
      iterator tmp(left);
      tmp -= off;
      return tmp;
   }

   friend difference_type operator-(const iterator& left, const iterator& right)
   {
      return void_ptr_ptr_cast(left.pn->up) - void_ptr_ptr_cast(right.pn->up);
   }

   //Comparison operators
   friend bool operator==   (const iterator& l, const iterator& r)
   {  return l.pn == r.pn;  }

   friend bool operator!=   (const iterator& l, const iterator& r)
   {  return l.pn != r.pn;  }

   friend bool operator<    (const iterator& l, const iterator& r)
   {  return void_ptr_ptr_cast(l.pn->up) < void_ptr_ptr_cast(r.pn->up);  }

   friend bool operator<=   (const iterator& l, const iterator& r)
   {  return void_ptr_ptr_cast(l.pn->up) <= void_ptr_ptr_cast(r.pn->up);  }

   friend bool operator>    (const iterator& l, const iterator& r)
   {  return void_ptr_ptr_cast(l.pn->up) > void_ptr_ptr_cast(r.pn->up);  }

   friend bool operator>=   (const iterator& l, const iterator& r)
   {  return void_ptr_ptr_cast(l.pn->up) >= void_ptr_ptr_cast(r.pn->up);  }

   node_type_ptr_t pn;
};

template<class A, unsigned int Version>
struct select_multiallocation_chain
{
   typedef typename A::multiallocation_chain type;
};

template<class A>
struct select_multiallocation_chain<A, 1>
{
   typedef typename boost::intrusive::pointer_traits
      <typename allocator_traits<A>::pointer>::
         template rebind_pointer<void>::type                void_ptr;
   typedef container_detail::basic_multiallocation_chain
      <void_ptr>                                            multialloc_cached_counted;
   typedef boost::container::container_detail::
      transform_multiallocation_chain
         < multialloc_cached_counted
         , typename allocator_traits<A>::value_type>        type;
};

} //namespace stable_vector_detail

#if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

#if defined(STABLE_VECTOR_ENABLE_INVARIANT_CHECKING)

#define STABLE_VECTOR_CHECK_INVARIANT \
invariant_checker BOOST_JOIN(check_invariant_,__LINE__)(*this); \
BOOST_JOIN(check_invariant_,__LINE__).touch();
#else

#define STABLE_VECTOR_CHECK_INVARIANT

#endif   //#if defined(STABLE_VECTOR_ENABLE_INVARIANT_CHECKING)

#endif   //#if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

/// @endcond

//!Originally developed by Joaquin M. Lopez Munoz, stable_vector is std::vector
//!drop-in replacement implemented as a node container, offering iterator and reference
//!stability.
//!
//!More details taken the author's blog: (<a href="http://bannalia.blogspot.com/2008/09/introducing-stablevector.html" > Introducing stable_vector</a>)
//!
//!We present stable_vector, a fully STL-compliant stable container that provides
//!most of the features of std::vector except element contiguity. 
//!
//!General properties: stable_vector satisfies all the requirements of a container,
//!a reversible container and a sequence and provides all the optional operations
//!present in std::vector. Like std::vector,  iterators are random access.
//!stable_vector does not provide element contiguity; in exchange for this absence,
//!the container is stable, i.e. references and iterators to an element of a stable_vector
//!remain valid as long as the element is not erased, and an iterator that has been
//!assigned the return value of end() always remain valid until the destruction of
//!the associated  stable_vector.
//!
//!Operation complexity: The big-O complexities of stable_vector operations match
//!exactly those of std::vector. In general, insertion/deletion is constant time at
//!the end of the sequence and linear elsewhere. Unlike std::vector, stable_vector
//!does not internally perform any value_type destruction, copy or assignment
//!operations other than those exactly corresponding to the insertion of new
//!elements or deletion of stored elements, which can sometimes compensate in terms
//!of performance for the extra burden of doing more pointer manipulation and an
//!additional allocation per element.
//!
//!Exception safety: As stable_vector does not internally copy elements around, some
//!operations provide stronger exception safety guarantees than in std::vector:
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class A = std::allocator<T> >
#else
template <class T, class A>
#endif
class stable_vector
{
   ///@cond
   typedef allocator_traits<A>                        allocator_traits_type;
   typedef typename container_detail::
      move_const_ref_type<T>::type                    insert_const_ref_type;
   typedef typename boost::intrusive::pointer_traits
      <typename allocator_traits_type::pointer>::
         template rebind_pointer<void>::type          void_ptr;
   typedef typename boost::intrusive::pointer_traits
      <void_ptr>::template
         rebind_pointer<const void>::type             const_void_ptr;
   typedef typename boost::intrusive::pointer_traits
      <void_ptr>::template
         rebind_pointer<void_ptr>::type               void_ptr_ptr;
   typedef typename boost::intrusive::pointer_traits
      <void_ptr>::template
         rebind_pointer<const void_ptr>::type         const_void_ptr_ptr;
   typedef stable_vector_detail::node_type
      <void_ptr, T>                                   node_type_t;
   typedef typename boost::intrusive::pointer_traits
      <void_ptr>::template
         rebind_pointer<node_type_t>::type            node_type_ptr_t;
   typedef stable_vector_detail::node_type_base
      <void_ptr>                                      node_type_base_t;
   typedef typename boost::intrusive::pointer_traits
      <void_ptr>::template
         rebind_pointer<node_type_base_t>::type       node_type_base_ptr_t;
   typedef ::boost::container::vector<void_ptr,
      typename allocator_traits_type::
         template portable_rebind_alloc
            <void_ptr>::type>                         impl_type;
   typedef typename impl_type::iterator               impl_iterator;
   typedef typename impl_type::const_iterator         const_impl_iterator;

   typedef ::boost::container::container_detail::
      integral_constant<unsigned, 1>                  allocator_v1;
   typedef ::boost::container::container_detail::
      integral_constant<unsigned, 2>                  allocator_v2;
   typedef ::boost::container::container_detail::integral_constant 
      <unsigned, boost::container::container_detail::
      version<A>::value>                              alloc_version;
   typedef typename allocator_traits_type::
      template portable_rebind_alloc
         <node_type_t>::type                          node_allocator_type;

   node_type_ptr_t allocate_one()
   {  return this->allocate_one(alloc_version());   }

   template<class AllocatorVersion>
   node_type_ptr_t allocate_one(AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {  return node_alloc().allocate(1);   }

   template<class AllocatorVersion>
   node_type_ptr_t allocate_one(AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <!boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {  return node_alloc().allocate_one();   }

   void deallocate_one(node_type_ptr_t p)
   {  return this->deallocate_one(p, alloc_version());   }

   template<class AllocatorVersion>
   void deallocate_one(node_type_ptr_t p, AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {  node_alloc().deallocate(p, 1);   }

   template<class AllocatorVersion>
   void deallocate_one(node_type_ptr_t p, AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <!boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {  node_alloc().deallocate_one(p);   }

   friend class stable_vector_detail::clear_on_destroy<stable_vector>;
   ///@endcond
   public:


   // types:

   typedef typename allocator_traits_type::reference              reference;
   typedef typename allocator_traits_type::const_reference        const_reference;
   typedef typename allocator_traits_type::pointer                pointer;
   typedef typename allocator_traits_type::const_pointer          const_pointer;
   typedef stable_vector_detail::iterator
      <T,T&, pointer>                                 iterator;
   typedef stable_vector_detail::iterator
      <T,const T&, const_pointer>                     const_iterator;
   typedef typename impl_type::size_type              size_type;
   typedef typename iterator::difference_type         difference_type;
   typedef T                                          value_type;
   typedef A                                          allocator_type;
   typedef std::reverse_iterator<iterator>            reverse_iterator;
   typedef std::reverse_iterator<const_iterator>      const_reverse_iterator;
   typedef node_allocator_type                        stored_allocator_type;

   ///@cond
   private:
   BOOST_COPYABLE_AND_MOVABLE(stable_vector)
   static const size_type ExtraPointers = 3;
   //This container stores metadata at the end of the void_ptr vector with additional 3 pointers:
   //    back() is impl.back() - ExtraPointers;
   //    end node index is impl.end()[-3]
   //    Node cache first is impl.end()[-2];
   //    Node cache last is  *impl.back();

   typedef typename stable_vector_detail::
      select_multiallocation_chain
      < node_allocator_type
      , alloc_version::value
      >::type                                         multiallocation_chain;
   ///@endcond
   public:

   //! <b>Effects</b>: Default constructs a stable_vector.
   //! 
   //! <b>Throws</b>: If allocator_type's default constructor throws.
   //! 
   //! <b>Complexity</b>: Constant.
   stable_vector()
      : internal_data(), impl()
   {
      STABLE_VECTOR_CHECK_INVARIANT;
   }

   //! <b>Effects</b>: Constructs a stable_vector taking the allocator as parameter.
   //! 
   //! <b>Throws</b>: If allocator_type's copy constructor throws.
   //! 
   //! <b>Complexity</b>: Constant.
   explicit stable_vector(const A& al)
      : internal_data(al),impl(al)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
   }

   //! <b>Effects</b>: Constructs a stable_vector that will use a copy of allocator a
   //!   and inserts n default contructed values.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's default or copy constructor throws.
   //! 
   //! <b>Complexity</b>: Linear to n.
   explicit stable_vector(size_type n)
      : internal_data(A()),impl(A())
   {
      stable_vector_detail::clear_on_destroy<stable_vector> cod(*this);
      this->resize(n);
      STABLE_VECTOR_CHECK_INVARIANT;
      cod.release();
   }

   //! <b>Effects</b>: Constructs a stable_vector that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's default or copy constructor throws.
   //! 
   //! <b>Complexity</b>: Linear to n.
   stable_vector(size_type n, const T& t, const A& al=A())
      : internal_data(al),impl(al)
   {
      stable_vector_detail::clear_on_destroy<stable_vector> cod(*this);
      this->insert(this->cbegin(), n, t);
      STABLE_VECTOR_CHECK_INVARIANT;
      cod.release();
   }

   //! <b>Effects</b>: Constructs a stable_vector that will use a copy of allocator a
   //!   and inserts a copy of the range [first, last) in the stable_vector.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's constructor taking an dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InputIterator>
   stable_vector(InputIterator first,InputIterator last,const A& al=A())
      : internal_data(al),impl(al)
   {
      stable_vector_detail::clear_on_destroy<stable_vector> cod(*this);
      this->insert(this->cbegin(), first, last);
      STABLE_VECTOR_CHECK_INVARIANT;
      cod.release();
   }

   //! <b>Effects</b>: Copy constructs a stable_vector.
   //!
   //! <b>Postcondition</b>: x == *this.
   //! 
   //! <b>Complexity</b>: Linear to the elements x contains.
   stable_vector(const stable_vector& x)
      : internal_data(allocator_traits<node_allocator_type>::
         select_on_container_copy_construction(x.node_alloc()))
      , impl(allocator_traits<allocator_type>::
         select_on_container_copy_construction(x.impl.get_stored_allocator()))
   {
      stable_vector_detail::clear_on_destroy<stable_vector> cod(*this);
      this->insert(this->cbegin(), x.begin(), x.end());
      STABLE_VECTOR_CHECK_INVARIANT;
      cod.release();
   }

   //! <b>Effects</b>: Move constructor. Moves mx's resources to *this.
   //!
   //! <b>Throws</b>: If allocator_type's copy constructor throws.
   //! 
   //! <b>Complexity</b>: Constant.
   stable_vector(BOOST_RV_REF(stable_vector) x)
      : internal_data(boost::move(x.node_alloc())), impl(boost::move(x.impl))
   {
      this->priv_swap_members(x);
   }

   //! <b>Effects</b>: Destroys the stable_vector. All stored values are destroyed
   //!   and used memory is deallocated.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements.
   ~stable_vector()
   {
      this->clear();
      clear_pool();  
   }

   //! <b>Effects</b>: Makes *this contain the same elements as x.
   //!
   //! <b>Postcondition</b>: this->size() == x.size(). *this contains a copy 
   //! of each of x's elements. 
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in x.
   stable_vector& operator=(BOOST_COPY_ASSIGN_REF(stable_vector) x)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      if (&x != this){
         node_allocator_type &this_alloc     = this->node_alloc();
         const node_allocator_type &x_alloc  = x.node_alloc();
         container_detail::bool_<allocator_traits_type::
            propagate_on_container_copy_assignment::value> flag;
         if(flag && this_alloc != x_alloc){
            this->clear();
            this->shrink_to_fit();
         }
         container_detail::assign_alloc(this->node_alloc(), x.node_alloc(), flag);
         container_detail::assign_alloc(this->impl.get_stored_allocator(), x.impl.get_stored_allocator(), flag);
         this->assign(x.begin(), x.end());
      }
      return *this;
   }

   //! <b>Effects</b>: Move assignment. All mx's values are transferred to *this.
   //!
   //! <b>Postcondition</b>: x.empty(). *this contains a the elements x had
   //!   before the function.
   //!
   //! <b>Throws</b>: If allocator_type's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear.
   stable_vector& operator=(BOOST_RV_REF(stable_vector) x)
   {
      if (&x != this){
         node_allocator_type &this_alloc = this->node_alloc();
         node_allocator_type &x_alloc    = x.node_alloc();
         //If allocators are equal we can just swap pointers
         if(this_alloc == x_alloc){
            //Destroy objects but retain memory
            this->clear();
            this->impl = boost::move(x.impl);
            this->priv_swap_members(x);
            //Move allocator if needed
            container_detail::bool_<allocator_traits_type::
               propagate_on_container_move_assignment::value> flag;
            container_detail::move_alloc(this->node_alloc(), x.node_alloc(), flag);
         }
         //If unequal allocators, then do a one by one move
         else{
            typedef typename std::iterator_traits<iterator>::iterator_category ItCat;
            this->assign( boost::make_move_iterator(x.begin())
                        , boost::make_move_iterator(x.end()));
         }
      }
      return *this;
   }

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's constructor from dereferencing InpIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template<typename InputIterator>
   void assign(InputIterator first,InputIterator last)
   {
      assign_dispatch(first, last, boost::is_integral<InputIterator>());
   }


   //! <b>Effects</b>: Assigns the n copies of val to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   void assign(size_type n,const T& t)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      return assign_dispatch(cvalue_iterator(t, n), cvalue_iterator(), boost::mpl::false_());
   }

   //! <b>Effects</b>: Returns a copy of the internal allocator.
   //! 
   //! <b>Throws</b>: If allocator's copy constructor throws.
   //! 
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator()const  {return node_alloc();}

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //! 
   //! <b>Throws</b>: Nothing
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT
   {  return node_alloc(); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //! 
   //! <b>Throws</b>: Nothing
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT
   {  return node_alloc(); }


   //! <b>Effects</b>: Returns an iterator to the first element contained in the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator  begin()
   {   return (impl.empty()) ? end(): iterator(node_ptr_cast(impl.front())) ;   }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator  begin()const
   {   return (impl.empty()) ? cend() : const_iterator(node_ptr_cast(impl.front())) ;   }

   //! <b>Effects</b>: Returns an iterator to the end of the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   iterator        end()                {return iterator(get_end_node());}

   //! <b>Effects</b>: Returns a const_iterator to the end of the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator  end()const           {return const_iterator(get_end_node());}

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning 
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator       rbegin()      {return reverse_iterator(this->end());}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin()const {return const_reverse_iterator(this->end());}

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reverse_iterator       rend()        {return reverse_iterator(this->begin());}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend()const   {return const_reverse_iterator(this->begin());}

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator         cbegin()const {return this->begin();}

   //! <b>Effects</b>: Returns a const_iterator to the end of the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_iterator         cend()const   {return this->end();}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning 
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin()const{return this->rbegin();}

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed stable_vector. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend()const  {return this->rend();}

   //! <b>Effects</b>: Returns the number of the elements contained in the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type size() const
   {  return impl.empty() ? 0 : (impl.size() - ExtraPointers);   }

   //! <b>Effects</b>: Returns the largest possible size of the stable_vector.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type max_size() const
   {  return impl.max_size() - ExtraPointers;  }

   //! <b>Effects</b>: Number of elements for which memory has been allocated.
   //!   capacity() is always greater than or equal to size().
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   size_type capacity() const
   {
      if(!impl.capacity()){
         return 0;
      }
      else{
         const size_type num_nodes = this->impl.size() + this->internal_data.pool_size;
         const size_type num_buck  = this->impl.capacity();
         return (num_nodes < num_buck) ? num_nodes : num_buck;
      }
   }

   //! <b>Effects</b>: Returns true if the stable_vector contains no elements.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   bool empty() const
   {  return impl.empty() || impl.size() == ExtraPointers;  }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are copy constructed from x.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type n, const T& t)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      if(n > size())
         this->insert(this->cend(), n - this->size(), t);
      else if(n < this->size())
         this->erase(this->cbegin() + n, this->cend());
   }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are default constructed.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type n)
   {
      typedef default_construct_iterator<value_type, difference_type> default_iterator;
      STABLE_VECTOR_CHECK_INVARIANT;
      if(n > size())
         this->insert(this->cend(), default_iterator(n - this->size()), default_iterator());
      else if(n < this->size())
         this->erase(this->cbegin() + n, this->cend());
   }

   //! <b>Effects</b>: If n is less than or equal to capacity(), this call has no
   //!   effect. Otherwise, it is a request for allocation of additional memory.
   //!   If the request is successful, then capacity() is greater than or equal to
   //!   n; otherwise, capacity() is unchanged. In either case, size() is unchanged.
   //! 
   //! <b>Throws</b>: If memory allocation allocation throws.
   void reserve(size_type n)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      if(n > this->max_size())
         throw std::bad_alloc();

      size_type size = this->size();   
      size_type old_capacity = this->capacity();
      if(n > old_capacity){
         this->initialize_end_node(n);
         const void * old_ptr = &impl[0];
         impl.reserve(n + ExtraPointers);
         bool realloced = &impl[0] != old_ptr;
         //Fix the pointers for the newly allocated buffer
         if(realloced){
            this->align_nodes(impl.begin(), impl.begin()+size+1);
         }
         //Now fill pool if data is not enough
         if((n - size) > this->internal_data.pool_size){
            this->add_to_pool((n - size) - this->internal_data.pool_size);
         }
      }
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element 
   //!   from the beginning of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reference operator[](size_type n){return value(impl[n]);}

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element 
   //!   from the beginning of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reference operator[](size_type n)const{return value(impl[n]);}

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element 
   //!   from the beginning of the container.
   //! 
   //! <b>Throws</b>: std::range_error if n >= size()
   //! 
   //! <b>Complexity</b>: Constant.
   reference at(size_type n)
   {
      if(n>=size())
         throw std::out_of_range("invalid subscript at stable_vector::at");
      return operator[](n);
   }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element 
   //!   from the beginning of the container.
   //! 
   //! <b>Throws</b>: std::range_error if n >= size()
   //! 
   //! <b>Complexity</b>: Constant.
   const_reference at(size_type n)const
   {
      if(n>=size())
         throw std::out_of_range("invalid subscript at stable_vector::at");
      return operator[](n);
   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the first
   //!   element of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reference front()
   {  return value(impl.front());   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first
   //!   element of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reference front()const
   {  return value(impl.front());   }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the last
   //!   element of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   reference back()
   {  return value(*(&impl.back() - ExtraPointers)); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the last
   //!   element of the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   const_reference back()const
   {  return value(*(&impl.back() - ExtraPointers)); }

   //! <b>Effects</b>: Inserts a copy of x at the end of the stable_vector.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(insert_const_ref_type x) 
   {  return priv_push_back(x);  }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   void push_back(T &x) { push_back(const_cast<const T &>(x)); }

   template<class U>
   void push_back(const U &u, typename container_detail::enable_if_c
                  <container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   { return priv_push_back(u); }
   #endif

   //! <b>Effects</b>: Constructs a new element in the end of the stable_vector
   //!   and moves the resources of mx to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(BOOST_RV_REF(T) t) 
   {  this->insert(end(), boost::move(t));  }

   //! <b>Effects</b>: Removes the last element from the stable_vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   void pop_back()
   {  this->erase(this->end()-1);   }

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of x before position.
   //!
   //! <b>Throws</b>: If memory allocation throws or x's copy constructor throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, insert_const_ref_type x) 
   {  return this->priv_insert(position, x); }

   #if defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   iterator insert(const_iterator position, T &x) { return this->insert(position, const_cast<const T &>(x)); }

   template<class U>
   iterator insert(const_iterator position, const U &u, typename container_detail::enable_if_c
                  <container_detail::is_same<T, U>::value && !::boost::has_move_emulation_enabled<U>::value >::type* =0)
   {  return this->priv_insert(position, u); }
   #endif

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a new element before position with mx's resources.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, BOOST_RV_REF(T) x) 
   {
      typedef repeat_iterator<T, difference_type>           repeat_it;
      typedef boost::move_iterator<repeat_it> repeat_move_it;
      //Just call more general insert(pos, size, value) and return iterator
      size_type pos_n = position - cbegin();
      this->insert(position
         ,repeat_move_it(repeat_it(x, 1))
         ,repeat_move_it(repeat_it()));
      return iterator(this->begin() + pos_n);
   }

   //! <b>Requires</b>: pos must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert n copies of x before pos.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   void insert(const_iterator position, size_type n, const T& t)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      this->insert_not_iter(position, n, t);
   }

   //! <b>Requires</b>: pos must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before pos.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InpIt throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to std::distance [first, last).
   template <class InputIterator>
   void insert(const_iterator position,InputIterator first, InputIterator last)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      this->insert_iter(position,first,last,
                        boost::mpl::not_<boost::is_integral<InputIterator> >());
   }

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the stable_vector.
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   template<class ...Args>
   void emplace_back(Args &&...args)
   {
      typedef emplace_functor<Args...>         EmplaceFunctor;
      typedef emplace_iterator<node_type_t, EmplaceFunctor, difference_type> EmplaceIterator;
      EmplaceFunctor &&ef = EmplaceFunctor(boost::forward<Args>(args)...);
      this->insert(this->cend(), EmplaceIterator(ef), EmplaceIterator());
   }

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... before position
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   template<class ...Args>
   iterator emplace(const_iterator position, Args && ...args)
   {
      //Just call more general insert(pos, size, value) and return iterator
      size_type pos_n = position - cbegin();
      typedef emplace_functor<Args...>         EmplaceFunctor;
      typedef emplace_iterator<node_type_t, EmplaceFunctor, difference_type> EmplaceIterator;
      EmplaceFunctor &&ef = EmplaceFunctor(boost::forward<Args>(args)...);
      this->insert(position, EmplaceIterator(ef), EmplaceIterator());
      return iterator(this->begin() + pos_n);
   }

   #else

   #define BOOST_PP_LOCAL_MACRO(n)                                                              \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)       \
   void emplace_back(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                        \
   {                                                                                            \
      typedef BOOST_PP_CAT(BOOST_PP_CAT(emplace_functor, n), arg)                               \
         BOOST_PP_EXPR_IF(n, <) BOOST_PP_ENUM_PARAMS(n, P) BOOST_PP_EXPR_IF(n, >)               \
            EmplaceFunctor;                                                                     \
      typedef emplace_iterator<node_type_t, EmplaceFunctor, difference_type>  EmplaceIterator;  \
      EmplaceFunctor ef BOOST_PP_LPAREN_IF(n)                                                   \
                        BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)                   \
                        BOOST_PP_RPAREN_IF(n);                                                  \
      this->insert(this->cend() , EmplaceIterator(ef), EmplaceIterator());                      \
   }                                                                                            \
                                                                                                \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)       \
   iterator emplace(const_iterator pos                                                          \
           BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                         \
   {                                                                                            \
      typedef BOOST_PP_CAT(BOOST_PP_CAT(emplace_functor, n), arg)                               \
         BOOST_PP_EXPR_IF(n, <) BOOST_PP_ENUM_PARAMS(n, P) BOOST_PP_EXPR_IF(n, >)               \
            EmplaceFunctor;                                                                     \
      typedef emplace_iterator<node_type_t, EmplaceFunctor, difference_type>  EmplaceIterator;  \
      EmplaceFunctor ef BOOST_PP_LPAREN_IF(n)                                                   \
                        BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _)                   \
                        BOOST_PP_RPAREN_IF(n);                                                  \
      size_type pos_n = pos - this->cbegin();                                                   \
      this->insert(pos, EmplaceIterator(ef), EmplaceIterator());                                \
      return iterator(this->begin() + pos_n);                                                   \
   }                                                                                            \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   //! <b>Effects</b>: Erases the element at position pos.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements between pos and the 
   //!   last element. Constant if pos is the last element.
   iterator erase(const_iterator position)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      difference_type d = position - this->cbegin();
      impl_iterator   it = impl.begin() + d;
      this->delete_node(*it);
      it = impl.erase(it);
      this->align_nodes(it, get_last_align());
      return this->begin()+d;
   }

   //! <b>Effects</b>: Erases the elements pointed by [first, last).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the distance between first and last
   //!   plus linear to the elements between pos and the last element.
   iterator erase(const_iterator first, const_iterator last)
   {   return priv_erase(first, last, alloc_version());  }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(stable_vector & x)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      container_detail::bool_<allocator_traits_type::propagate_on_container_swap::value> flag;
      container_detail::swap_alloc(this->node_alloc(), x.node_alloc(), flag);
      //vector's allocator is swapped here
      this->impl.swap(x.impl);
      this->priv_swap_members(x);
   }

   //! <b>Effects</b>: Erases all the elements of the stable_vector.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the stable_vector.
   void clear()
   {   this->erase(this->cbegin(),this->cend()); }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //!   with previous allocations. The size of the stable_vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit()
   {
      if(this->capacity()){
         //First empty allocated node pool
         this->clear_pool();
         //If empty completely destroy the index, let's recover default-constructed state
         if(this->empty()){
            this->impl.clear();
            this->impl.shrink_to_fit();
            this->internal_data.set_end_pointer_to_default_constructed();
         }
         //Otherwise, try to shrink-to-fit the index and readjust pointers if necessary
         else{
            const size_type size = this->size();
            const void* old_ptr = &impl[0];
            this->impl.shrink_to_fit();
            bool realloced = &impl[0] != old_ptr;
            //Fix the pointers for the newly allocated buffer
            if(realloced){
               this->align_nodes(impl.begin(), impl.begin()+size+1);
            }
         }
      }
   }

   /// @cond

   iterator priv_insert(const_iterator position, const value_type &t)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      return this->insert_iter(position, cvalue_iterator(t, 1), cvalue_iterator(), std::forward_iterator_tag());
   }

   void priv_push_back(const value_type &t)
   {  this->insert(end(), t);  }

   template<class AllocatorVersion>
   void clear_pool(AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {
      if(!impl.empty() && impl.back()){
         void_ptr &pool_first_ref = impl.end()[-2];
         void_ptr &pool_last_ref = impl.back();

         multiallocation_chain holder;
         holder.incorporate_after(holder.before_begin(), pool_first_ref, pool_last_ref, this->internal_data.pool_size);
         while(!holder.empty()){
            node_type_ptr_t n = holder.front();
            holder.pop_front();
            this->deallocate_one(n);
         }
         pool_first_ref = pool_last_ref = 0;
         this->internal_data.pool_size = 0;
      }
   }

   template<class AllocatorVersion>
   void clear_pool(AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <!boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {
      if(!impl.empty() && impl.back()){
         void_ptr &pool_first_ref = impl.end()[-2];
         void_ptr &pool_last_ref = impl.back();
         multiallocation_chain holder;
         holder.incorporate_after(holder.before_begin(), pool_first_ref, pool_last_ref, internal_data.pool_size);
         node_alloc().deallocate_individual(boost::move(holder));
         pool_first_ref = pool_last_ref = 0;
         this->internal_data.pool_size = 0;
      }
   }

   void clear_pool()
   {
      this->clear_pool(alloc_version());
   }

   void add_to_pool(size_type n)
   {
      this->add_to_pool(n, alloc_version());
   }

   template<class AllocatorVersion>
   void add_to_pool(size_type n, AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {
      size_type remaining = n;
      while(remaining--){
         this->put_in_pool(this->allocate_one());
      }
   }

   template<class AllocatorVersion>
   void add_to_pool(size_type n, AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <!boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {
      void_ptr &pool_first_ref = impl.end()[-2];
      void_ptr &pool_last_ref = impl.back();
      multiallocation_chain holder;
      holder.incorporate_after(holder.before_begin(), pool_first_ref, pool_last_ref, internal_data.pool_size);
      //BOOST_STATIC_ASSERT((::boost::has_move_emulation_enabled<multiallocation_chain>::value == true));
      multiallocation_chain m (node_alloc().allocate_individual(n));
      holder.splice_after(holder.before_begin(), m, m.before_begin(), m.last(), n);
      this->internal_data.pool_size += n;
      std::pair<void_ptr, void_ptr> data(holder.extract_data());
      pool_first_ref = data.first;
      pool_last_ref = data.second;
   }

   void put_in_pool(node_type_ptr_t p)
   {
      void_ptr &pool_first_ref = impl.end()[-2];
      void_ptr &pool_last_ref = impl.back();
      multiallocation_chain holder;
      holder.incorporate_after(holder.before_begin(), pool_first_ref, pool_last_ref, internal_data.pool_size);
      holder.push_front(p);
      ++this->internal_data.pool_size;
      std::pair<void_ptr, void_ptr> ret(holder.extract_data());
      pool_first_ref = ret.first;
      pool_last_ref = ret.second;
   }

   node_type_ptr_t get_from_pool()
   {
      if(!impl.back()){
         return node_type_ptr_t(0);
      }
      else{
         void_ptr &pool_first_ref = impl.end()[-2];
         void_ptr &pool_last_ref = impl.back();
         multiallocation_chain holder;
         holder.incorporate_after(holder.before_begin(), pool_first_ref, pool_last_ref, internal_data.pool_size);
         node_type_ptr_t ret = holder.front();
         holder.pop_front();
         --this->internal_data.pool_size;
         if(!internal_data.pool_size){
            pool_first_ref = pool_last_ref = void_ptr(0);
         }
         else{
            std::pair<void_ptr, void_ptr> data(holder.extract_data());
            pool_first_ref = data.first;
            pool_last_ref = data.second;
         }
         return ret;
      }
   }

   void insert_iter_prolog(size_type n, difference_type d)
   {
      initialize_end_node(n);
      const void* old_ptr = &impl[0];
      //size_type old_capacity = capacity();
      //size_type old_size = size();
      impl.insert(impl.begin()+d, n, 0);
      bool realloced = &impl[0] != old_ptr;
      //Fix the pointers for the newly allocated buffer
      if(realloced){
         align_nodes(impl.begin(), impl.begin()+d);
      }
   }

   template<typename InputIterator>
   void assign_dispatch(InputIterator first, InputIterator last, boost::mpl::false_)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      iterator first1   = this->begin();
      iterator last1    = this->end();
      for ( ; first1 != last1 && first != last; ++first1, ++first)
         *first1 = *first;
      if (first == last){
         this->erase(first1, last1);
      }
      else{
         this->insert(last1, first, last);
      }
   }

   template<typename Integer>
   void assign_dispatch(Integer n, Integer t, boost::mpl::true_)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      this->assign_dispatch(cvalue_iterator(t, n), cvalue_iterator(), boost::mpl::false_());
   }

   iterator priv_erase(const_iterator first, const_iterator last, allocator_v1)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      difference_type d1 = first - this->cbegin(), d2 = last - this->cbegin();
      if(d1 != d2){
         impl_iterator it1(impl.begin() + d1), it2(impl.begin() + d2);
         for(impl_iterator it = it1; it != it2; ++it)
            this->delete_node(*it);
         impl_iterator e = impl.erase(it1, it2);
         this->align_nodes(e, get_last_align());
      }
      return iterator(this->begin() + d1);
   }

   impl_iterator get_last_align()
   {
      return impl.end() - (ExtraPointers - 1);
   }

   const_impl_iterator get_last_align() const
   {
      return impl.cend() - (ExtraPointers - 1);
   }

   template<class AllocatorVersion>
   iterator priv_erase(const_iterator first, const_iterator last, AllocatorVersion,
      typename boost::container::container_detail::enable_if_c
         <!boost::container::container_detail::is_same<AllocatorVersion, allocator_v1>
            ::value>::type * = 0)
   {
      STABLE_VECTOR_CHECK_INVARIANT;
      return priv_erase(first, last, allocator_v1());
   }

   static node_type_ptr_t node_ptr_cast(const void_ptr &p)
   {
      return node_type_ptr_t(static_cast<node_type_t*>(stable_vector_detail::to_raw_pointer(p)));
   }

   static node_type_base_ptr_t node_base_ptr_cast(const void_ptr &p)
   {
      return node_type_base_ptr_t(static_cast<node_type_base_t*>(stable_vector_detail::to_raw_pointer(p)));
   }

   static value_type& value(const void_ptr &p)
   {
      return node_ptr_cast(p)->value;
   }

   void initialize_end_node(size_type impl_capacity = 0)
   {
      if(impl.empty()){
         impl.reserve(impl_capacity + ExtraPointers);
         impl.resize (ExtraPointers, void_ptr(0));
         impl[0] = &this->internal_data.end_node;
         this->internal_data.end_node.up = &impl[0];
      }
   }

   void readjust_end_node()
   {
      if(!this->impl.empty()){
         void_ptr &end_node_ref = *(this->get_last_align()-1);
         end_node_ref = this->get_end_node();
         this->internal_data.end_node.up = &end_node_ref;
      }
      else{
         this->internal_data.end_node.up = void_ptr(&this->internal_data.end_node.up);
      }
   }

   node_type_ptr_t get_end_node() const
   {
      const node_type_base_t* cp = &this->internal_data.end_node;
      node_type_base_t* p = const_cast<node_type_base_t*>(cp);
      return node_ptr_cast(p);
   }

   template<class Iter>
   void_ptr new_node(const void_ptr &up, Iter it)
   {
      node_type_ptr_t p = this->allocate_one();
      try{
         boost::container::construct_in_place(this->node_alloc(), &*p, it);
         p->set_pointer(up);
      }
      catch(...){
         this->deallocate_one(p);
         throw;
      }
      return p;
   }

   void delete_node(const void_ptr &p)
   {
      node_type_ptr_t n(node_ptr_cast(p));
      allocator_traits<node_allocator_type>::
         destroy(this->node_alloc(), container_detail::to_raw_pointer(n));
      this->put_in_pool(n);
   }

   static void align_nodes(impl_iterator first, impl_iterator last)
   {
      while(first!=last){
         node_ptr_cast(*first)->up = void_ptr(&*first);
         ++first;
      }
   }

   void insert_not_iter(const_iterator position, size_type n, const T& t)
   {
      typedef constant_iterator<value_type, difference_type> cvalue_iterator;
      this->insert_iter(position, cvalue_iterator(t, n), cvalue_iterator(), std::forward_iterator_tag());
   }

   template <class InputIterator>
   void insert_iter(const_iterator position,InputIterator first,InputIterator last, boost::mpl::true_)
   {
      typedef typename std::iterator_traits<InputIterator>::iterator_category category;
      this->insert_iter(position, first, last, category());
   }

   template <class InputIterator>
   void insert_iter(const_iterator position,InputIterator first,InputIterator last,std::input_iterator_tag)
   {
      for(; first!=last; ++first){
         this->insert(position, *first);
      }    
   }

   template <class InputIterator>
   iterator insert_iter(const_iterator position, InputIterator first, InputIterator last, std::forward_iterator_tag)
   {
      size_type       n = (size_type)std::distance(first,last);
      difference_type d = position-this->cbegin();
      if(n){
         this->insert_iter_prolog(n, d);
         const impl_iterator it(impl.begin() + d);
         this->insert_iter_fwd(it, first, last, n);
         //Fix the pointers for the newly allocated buffer
         this->align_nodes(it + n, get_last_align());
      }
      return this->begin() + d;
   }

   template <class FwdIterator>
   void insert_iter_fwd_alloc(const impl_iterator it, FwdIterator first, FwdIterator last, difference_type n, allocator_v1)
   {
      size_type i=0;
      try{
         while(first!=last){
            it[i] = this->new_node(void_ptr_ptr(&it[i]), first);
            ++first;
            ++i;
         }
      }
      catch(...){
         impl_iterator e = impl.erase(it + i, it + n);
         this->align_nodes(e, get_last_align());
         throw;
      }
   }

   template <class FwdIterator>
   void insert_iter_fwd_alloc(const impl_iterator it, FwdIterator first, FwdIterator last, difference_type n, allocator_v2)
   {
      multiallocation_chain mem(node_alloc().allocate_individual(n));

      size_type i = 0;
      node_type_ptr_t p = 0;
      try{
         while(first != last){
            p = mem.front();
            mem.pop_front();
            //This can throw
            boost::container::construct_in_place(this->node_alloc(), &*p, first);
            p->set_pointer(void_ptr_ptr(&it[i]));
            ++first;
            it[i] = p;
            ++i;
         }
      }
      catch(...){
         node_alloc().deallocate_one(p);
         node_alloc().deallocate_many(boost::move(mem));
         impl_iterator e = impl.erase(it+i, it+n);
         this->align_nodes(e, get_last_align());
         throw;
      }
   }

   template <class FwdIterator>
   void insert_iter_fwd(const impl_iterator it, FwdIterator first, FwdIterator last, difference_type n)
   {
      size_type i = 0;
      node_type_ptr_t p = 0;
      try{
         while(first != last){
            p = this->get_from_pool();
            if(!p){
               insert_iter_fwd_alloc(it+i, first, last, n-i, alloc_version());
               break;
            }
            //This can throw
            boost::container::construct_in_place(this->node_alloc(), &*p, first);
            p->set_pointer(void_ptr_ptr(&it[i]));
            ++first;
            it[i]=p;
            ++i;
         }
      }
      catch(...){
         put_in_pool(p);
         impl_iterator e = impl.erase(it+i, it+n);
         this->align_nodes(e, get_last_align());
         throw;
      }
   }

   template <class InputIterator>
   void insert_iter(const_iterator position, InputIterator first, InputIterator last, boost::mpl::false_)
   {
      this->insert_not_iter(position, first, last);
   }

   #if defined(STABLE_VECTOR_ENABLE_INVARIANT_CHECKING)
   bool invariant()const
   {
      if(impl.empty())
         return !capacity() && !size();
      if(get_end_node() != *(impl.end() - ExtraPointers)){
         return false;
      }
      for(const_impl_iterator it = impl.begin(), it_end = get_last_align(); it != it_end; ++it){
         if(const_void_ptr(node_ptr_cast(*it)->up) != 
               const_void_ptr(const_void_ptr_ptr(&*it)))
            return false;
      }
      size_type n = capacity()-size();
      const void_ptr &pool_head = impl.back();
      size_type num_pool = 0;
      node_type_ptr_t p = node_ptr_cast(pool_head);
      while(p){
         ++num_pool;
         p = node_ptr_cast(p->up);
      }
      return n >= num_pool;
   }

   class invariant_checker
   {
      invariant_checker(const invariant_checker &);
      invariant_checker & operator=(const invariant_checker &);
      const stable_vector* p;

      public:
      invariant_checker(const stable_vector& v):p(&v){}
      ~invariant_checker(){BOOST_ASSERT(p->invariant());}
      void touch(){}
   };
   #endif

   class ebo_holder
      : public node_allocator_type
   {
      private:
      BOOST_MOVABLE_BUT_NOT_COPYABLE(ebo_holder)
      public:
/*
      explicit ebo_holder(BOOST_RV_REF(ebo_holder) x)
         : node_allocator_type(boost::move(static_cast<node_allocator_type&>(x)))
         , pool_size(0)
         , end_node()
      {}
*/
      template<class AllocatorRLValue>
      explicit ebo_holder(BOOST_FWD_REF(AllocatorRLValue) a)
         : node_allocator_type(boost::forward<AllocatorRLValue>(a))
         , pool_size(0)
         , end_node()
      {
         this->set_end_pointer_to_default_constructed();
      }

      ebo_holder()
         : node_allocator_type()
         , pool_size(0)
         , end_node()
      {
         this->set_end_pointer_to_default_constructed();
      }

      void set_end_pointer_to_default_constructed()
      {
         end_node.set_pointer(void_ptr(&end_node.up));
      }

      size_type pool_size;
      node_type_base_t end_node;
   } internal_data;

   void priv_swap_members(stable_vector &x)
   {
      container_detail::do_swap(this->internal_data.pool_size, x.internal_data.pool_size);
      this->readjust_end_node();
      x.readjust_end_node();
   }

   node_allocator_type &node_alloc()              { return internal_data;  }
   const node_allocator_type &node_alloc() const  { return internal_data;  }

   impl_type                           impl;
   /// @endcond
};

template <typename T,typename A>
bool operator==(const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return x.size()==y.size()&&std::equal(x.begin(),x.end(),y.begin());
}

template <typename T,typename A>
bool operator< (const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return std::lexicographical_compare(x.begin(),x.end(),y.begin(),y.end());
}

template <typename T,typename A>
bool operator!=(const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return !(x==y);
}

template <typename T,typename A>
bool operator> (const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return y<x;
}

template <typename T,typename A>
bool operator>=(const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return !(x<y);
}

template <typename T,typename A>
bool operator<=(const stable_vector<T,A>& x,const stable_vector<T,A>& y)
{
   return !(x>y);
}

// specialized algorithms:

template <typename T, typename A>
void swap(stable_vector<T,A>& x,stable_vector<T,A>& y)
{
   x.swap(y);
}

/// @cond

#undef STABLE_VECTOR_CHECK_INVARIANT

/// @endcond

}}

#include <boost/container/detail/config_end.hpp>

#endif   //BOOST_CONTAINER_STABLE_VECTOR_HPP
