//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2012. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DEQUE_HPP
#define BOOST_CONTAINER_DEQUE_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/algorithms.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/allocator_traits.hpp>
#include <boost/container/container_fwd.hpp>
#include <boost/container/throw_exception.hpp>
#include <cstddef>
#include <iterator>
#include <boost/assert.hpp>
#include <memory>
#include <algorithm>
#include <boost/detail/no_exceptions_support.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/type_traits/has_trivial_copy.hpp>
#include <boost/type_traits/has_trivial_assign.hpp>
#include <boost/type_traits/has_nothrow_copy.hpp>
#include <boost/type_traits/has_nothrow_assign.hpp>
#include <boost/move/utility.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/detail/move_helpers.hpp>
#include <boost/container/detail/advanced_insert_int.hpp>
#include <boost/detail/no_exceptions_support.hpp>

namespace boost {
namespace container {

/// @cond
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Allocator = std::allocator<T> >
#else
template <class T, class Allocator>
#endif
class deque;

template <class T>
struct deque_value_traits
{
   typedef T value_type;
   static const bool trivial_dctr = boost::has_trivial_destructor<value_type>::value;
   static const bool trivial_dctr_after_move = ::boost::has_trivial_destructor_after_move<value_type>::value;
   static const bool trivial_copy = has_trivial_copy<value_type>::value;
   static const bool nothrow_copy = has_nothrow_copy<value_type>::value;
   static const bool trivial_assign = has_trivial_assign<value_type>::value;
   //static const bool nothrow_assign = has_nothrow_assign<value_type>::value;
   static const bool nothrow_assign = false;
};

// Note: this function is simply a kludge to work around several compilers'
//  bugs in handling constant expressions.
template<class T>
struct deque_buf_size
{
   static const std::size_t min_size = 512u;
   static const std::size_t sizeof_t = sizeof(T);
   static const std::size_t value    = sizeof_t < min_size ? (min_size/sizeof_t) : std::size_t(1);
};

namespace container_detail {

// Class invariants:
//  For any nonsingular iterator i:
//    i.node is the address of an element in the map array.  The
//      contents of i.node is a pointer to the beginning of a node.
//    i.first == //(i.node)
//    i.last  == i.first + node_size
//    i.cur is a pointer in the range [i.first, i.last).  NOTE:
//      the implication of this is that i.cur is always a dereferenceable
//      pointer, even if i is a past-the-end iterator.
//  Start and Finish are always nonsingular iterators.  NOTE: this means
//    that an empty deque must have one node, and that a deque
//    with N elements, where N is the buffer size, must have two nodes.
//  For every node other than start.node and finish.node, every element
//    in the node is an initialized object.  If start.node == finish.node,
//    then [start.cur, finish.cur) are initialized objects, and
//    the elements outside that range are uninitialized storage.  Otherwise,
//    [start.cur, start.last) and [finish.first, finish.cur) are initialized
//    objects, and [start.first, start.cur) and [finish.cur, finish.last)
//    are uninitialized storage.
//  [map, map + map_size) is a valid, non-empty range. 
//  [start.node, finish.node] is a valid range contained within
//    [map, map + map_size). 
//  Allocator pointer in the range [map, map + map_size) points to an allocated node
//    if and only if the pointer is in the range [start.node, finish.node].
template<class Pointer, bool IsConst>
class deque_iterator
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
   typedef typename if_c
      < IsConst
      , const value_type&
      , value_type&
      >::type                                                                       reference;

   static std::size_t s_buffer_size()
      { return deque_buf_size<value_type>::value; }

   typedef Pointer                                                                  val_alloc_ptr;
   typedef typename boost::intrusive::pointer_traits<Pointer>::
      template rebind_pointer<Pointer>::type                                        index_pointer;                              

   Pointer m_cur;
   Pointer m_first;
   Pointer m_last;
   index_pointer  m_node;

   public:

   Pointer get_cur()          const  {  return m_cur;  }
   Pointer get_first()        const  {  return m_first;  }
   Pointer get_last()         const  {  return m_last;  }
   index_pointer get_node()   const  {  return m_node;  }

   deque_iterator(val_alloc_ptr x, index_pointer y) BOOST_CONTAINER_NOEXCEPT
      : m_cur(x), m_first(*y), m_last(*y + s_buffer_size()), m_node(y)
   {}

   deque_iterator() BOOST_CONTAINER_NOEXCEPT
      : m_cur(), m_first(), m_last(), m_node()
   {}

   deque_iterator(deque_iterator<Pointer, false> const& x) BOOST_CONTAINER_NOEXCEPT
      : m_cur(x.get_cur()), m_first(x.get_first()), m_last(x.get_last()), m_node(x.get_node())
   {}

   deque_iterator(Pointer cur, Pointer first, Pointer last, index_pointer node) BOOST_CONTAINER_NOEXCEPT
      : m_cur(cur), m_first(first), m_last(last), m_node(node)
   {}

   deque_iterator<Pointer, false> unconst() const BOOST_CONTAINER_NOEXCEPT
   {
      return deque_iterator<Pointer, false>(this->get_cur(), this->get_first(), this->get_last(), this->get_node());
   }

   reference operator*() const BOOST_CONTAINER_NOEXCEPT
      { return *this->m_cur; }

   pointer operator->() const BOOST_CONTAINER_NOEXCEPT
      { return this->m_cur; }

   difference_type operator-(const deque_iterator& x) const BOOST_CONTAINER_NOEXCEPT
   {
      if(!this->m_cur && !x.m_cur){
         return 0;
      }
      return difference_type(this->s_buffer_size()) * (this->m_node - x.m_node - 1) +
         (this->m_cur - this->m_first) + (x.m_last - x.m_cur);
   }

   deque_iterator& operator++() BOOST_CONTAINER_NOEXCEPT
   {
      ++this->m_cur;
      if (this->m_cur == this->m_last) {
         this->priv_set_node(this->m_node + 1);
         this->m_cur = this->m_first;
      }
      return *this;
   }

   deque_iterator operator++(int) BOOST_CONTAINER_NOEXCEPT
   {
      deque_iterator tmp(*this);
      ++*this;
      return tmp;
   }

   deque_iterator& operator--() BOOST_CONTAINER_NOEXCEPT
   {
      if (this->m_cur == this->m_first) {
         this->priv_set_node(this->m_node - 1);
         this->m_cur = this->m_last;
      }
      --this->m_cur;
      return *this;
   }

   deque_iterator operator--(int) BOOST_CONTAINER_NOEXCEPT
   {
      deque_iterator tmp(*this);
      --*this;
      return tmp;
   }

   deque_iterator& operator+=(difference_type n) BOOST_CONTAINER_NOEXCEPT
   {
      difference_type offset = n + (this->m_cur - this->m_first);
      if (offset >= 0 && offset < difference_type(this->s_buffer_size()))
         this->m_cur += n;
      else {
         difference_type node_offset =
         offset > 0 ? offset / difference_type(this->s_buffer_size())
                     : -difference_type((-offset - 1) / this->s_buffer_size()) - 1;
         this->priv_set_node(this->m_node + node_offset);
         this->m_cur = this->m_first +
         (offset - node_offset * difference_type(this->s_buffer_size()));
      }
      return *this;
   }

   deque_iterator operator+(difference_type n) const BOOST_CONTAINER_NOEXCEPT
      {  deque_iterator tmp(*this); return tmp += n;  }

   deque_iterator& operator-=(difference_type n) BOOST_CONTAINER_NOEXCEPT
      { return *this += -n; }
   
   deque_iterator operator-(difference_type n) const BOOST_CONTAINER_NOEXCEPT
      {  deque_iterator tmp(*this); return tmp -= n;  }

   reference operator[](difference_type n) const BOOST_CONTAINER_NOEXCEPT
      { return *(*this + n); }

   friend bool operator==(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      { return l.m_cur == r.m_cur; }

   friend bool operator!=(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      { return l.m_cur != r.m_cur; }

   friend bool operator<(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      {  return (l.m_node == r.m_node) ? (l.m_cur < r.m_cur) : (l.m_node < r.m_node);  }

   friend bool operator>(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      { return r < l; }

   friend bool operator<=(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      { return !(r < l); }

   friend bool operator>=(const deque_iterator& l, const deque_iterator& r) BOOST_CONTAINER_NOEXCEPT
      { return !(l < r); }

   void priv_set_node(index_pointer new_node) BOOST_CONTAINER_NOEXCEPT
   {
      this->m_node = new_node;
      this->m_first = *new_node;
      this->m_last = this->m_first + this->s_buffer_size();
   }

   friend deque_iterator operator+(difference_type n, deque_iterator x) BOOST_CONTAINER_NOEXCEPT
      {  return x += n;  }
};

}  //namespace container_detail {

// Deque base class.  It has two purposes.  First, its constructor
//  and destructor allocate (but don't initialize) storage.  This makes
//  exception safety easier.
template <class Allocator>
class deque_base
{
   BOOST_COPYABLE_AND_MOVABLE(deque_base)
   public:
   typedef allocator_traits<Allocator>                            val_alloc_traits_type;
   typedef typename val_alloc_traits_type::value_type             val_alloc_val;
   typedef typename val_alloc_traits_type::pointer                val_alloc_ptr;
   typedef typename val_alloc_traits_type::const_pointer          val_alloc_cptr;
   typedef typename val_alloc_traits_type::reference              val_alloc_ref;
   typedef typename val_alloc_traits_type::const_reference        val_alloc_cref;
   typedef typename val_alloc_traits_type::difference_type        val_alloc_diff;
   typedef typename val_alloc_traits_type::size_type              val_alloc_size;
   typedef typename val_alloc_traits_type::template
      portable_rebind_alloc<val_alloc_ptr>::type                  ptr_alloc_t;
   typedef allocator_traits<ptr_alloc_t>                          ptr_alloc_traits_type;
   typedef typename ptr_alloc_traits_type::value_type             ptr_alloc_val;
   typedef typename ptr_alloc_traits_type::pointer                ptr_alloc_ptr;
   typedef typename ptr_alloc_traits_type::const_pointer          ptr_alloc_cptr;
   typedef typename ptr_alloc_traits_type::reference              ptr_alloc_ref;
   typedef typename ptr_alloc_traits_type::const_reference        ptr_alloc_cref;
   typedef Allocator                                                      allocator_type;
   typedef allocator_type                                         stored_allocator_type;
   typedef val_alloc_size                                         size_type;

   protected:

   typedef deque_value_traits<val_alloc_val>             traits_t;
   typedef ptr_alloc_t                                   map_allocator_type;

   static size_type s_buffer_size() BOOST_CONTAINER_NOEXCEPT
      { return deque_buf_size<val_alloc_val>::value; }

   val_alloc_ptr priv_allocate_node()
      {  return this->alloc().allocate(s_buffer_size());  }

   void priv_deallocate_node(val_alloc_ptr p) BOOST_CONTAINER_NOEXCEPT
      {  this->alloc().deallocate(p, s_buffer_size());  }

   ptr_alloc_ptr priv_allocate_map(size_type n)
      { return this->ptr_alloc().allocate(n); }

   void priv_deallocate_map(ptr_alloc_ptr p, size_type n) BOOST_CONTAINER_NOEXCEPT
      { this->ptr_alloc().deallocate(p, n); }

   typedef container_detail::deque_iterator<val_alloc_ptr, false> iterator;
   typedef container_detail::deque_iterator<val_alloc_ptr, true > const_iterator;

   deque_base(size_type num_elements, const allocator_type& a)
      :  members_(a)
   { this->priv_initialize_map(num_elements); }

   explicit deque_base(const allocator_type& a)
      :  members_(a)
   {}

   deque_base()
      :  members_()
   {}

   explicit deque_base(BOOST_RV_REF(deque_base) x)
      :  members_( boost::move(x.ptr_alloc())
                 , boost::move(x.alloc()) )
   {}

   ~deque_base()
   {
      if (this->members_.m_map) {
         this->priv_destroy_nodes(this->members_.m_start.m_node, this->members_.m_finish.m_node + 1);
         this->priv_deallocate_map(this->members_.m_map, this->members_.m_map_size);
      }
   }

   private:
   deque_base(const deque_base&);
 
   protected:

   void swap_members(deque_base &x) BOOST_CONTAINER_NOEXCEPT
   {
      std::swap(this->members_.m_start, x.members_.m_start);
      std::swap(this->members_.m_finish, x.members_.m_finish);
      std::swap(this->members_.m_map, x.members_.m_map);
      std::swap(this->members_.m_map_size, x.members_.m_map_size);
   }

   void priv_initialize_map(size_type num_elements)
   {
//      if(num_elements){
         size_type num_nodes = num_elements / s_buffer_size() + 1;

         this->members_.m_map_size = container_detail::max_value((size_type) InitialMapSize, num_nodes + 2);
         this->members_.m_map = this->priv_allocate_map(this->members_.m_map_size);

         ptr_alloc_ptr nstart = this->members_.m_map + (this->members_.m_map_size - num_nodes) / 2;
         ptr_alloc_ptr nfinish = nstart + num_nodes;
            
         BOOST_TRY {
            this->priv_create_nodes(nstart, nfinish);
         }
         BOOST_CATCH(...){
            this->priv_deallocate_map(this->members_.m_map, this->members_.m_map_size);
            this->members_.m_map = 0;
            this->members_.m_map_size = 0;
            BOOST_RETHROW
         }
         BOOST_CATCH_END

         this->members_.m_start.priv_set_node(nstart);
         this->members_.m_finish.priv_set_node(nfinish - 1);
         this->members_.m_start.m_cur = this->members_.m_start.m_first;
         this->members_.m_finish.m_cur = this->members_.m_finish.m_first +
                        num_elements % s_buffer_size();
//      }
   }

   void priv_create_nodes(ptr_alloc_ptr nstart, ptr_alloc_ptr nfinish)
   {
      ptr_alloc_ptr cur;
      BOOST_TRY {
         for (cur = nstart; cur < nfinish; ++cur)
            *cur = this->priv_allocate_node();
      }
      BOOST_CATCH(...){
         this->priv_destroy_nodes(nstart, cur);
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   void priv_destroy_nodes(ptr_alloc_ptr nstart, ptr_alloc_ptr nfinish) BOOST_CONTAINER_NOEXCEPT
   {
      for (ptr_alloc_ptr n = nstart; n < nfinish; ++n)
         this->priv_deallocate_node(*n);
   }

   void priv_clear_map() BOOST_CONTAINER_NOEXCEPT
   {
      if (this->members_.m_map) {
         this->priv_destroy_nodes(this->members_.m_start.m_node, this->members_.m_finish.m_node + 1);
         this->priv_deallocate_map(this->members_.m_map, this->members_.m_map_size);
         this->members_.m_map = 0;
         this->members_.m_map_size = 0;
         this->members_.m_start = iterator();
         this->members_.m_finish = this->members_.m_start;
      }
   }

   enum { InitialMapSize = 8 };

   protected:
   struct members_holder
      :  public ptr_alloc_t
      ,  public allocator_type
   {
      members_holder()
         :  map_allocator_type(), allocator_type()
         ,  m_map(0), m_map_size(0)
         ,  m_start(), m_finish(m_start)
      {}

      explicit members_holder(const allocator_type &a)
         :  map_allocator_type(a), allocator_type(a)
         ,  m_map(0), m_map_size(0)
         ,  m_start(), m_finish(m_start)
      {}

      template<class ValAllocConvertible, class PtrAllocConvertible>
      members_holder(BOOST_FWD_REF(PtrAllocConvertible) pa, BOOST_FWD_REF(ValAllocConvertible) va)
         : map_allocator_type(boost::forward<PtrAllocConvertible>(pa))
         , allocator_type    (boost::forward<ValAllocConvertible>(va))
         , m_map(0), m_map_size(0)
         , m_start(), m_finish(m_start)
      {}

      ptr_alloc_ptr   m_map;
      val_alloc_size  m_map_size;
      iterator        m_start;
      iterator        m_finish;
   } members_;

   ptr_alloc_t &ptr_alloc() BOOST_CONTAINER_NOEXCEPT
   {  return members_;  }
  
   const ptr_alloc_t &ptr_alloc() const BOOST_CONTAINER_NOEXCEPT
   {  return members_;  }

   allocator_type &alloc() BOOST_CONTAINER_NOEXCEPT
   {  return members_;  }
  
   const allocator_type &alloc() const BOOST_CONTAINER_NOEXCEPT
   {  return members_;  }
};
/// @endcond

//! Deque class
//!
#ifdef BOOST_CONTAINER_DOXYGEN_INVOKED
template <class T, class Allocator = std::allocator<T> >
#else
template <class T, class Allocator>
#endif
class deque : protected deque_base<Allocator>
{
   /// @cond
   private:
   typedef deque_base<Allocator> Base;
   /// @endcond

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
   typedef BOOST_CONTAINER_IMPDEF(allocator_type)                                      stored_allocator_type;
   typedef BOOST_CONTAINER_IMPDEF(typename Base::iterator)                             iterator;
   typedef BOOST_CONTAINER_IMPDEF(typename Base::const_iterator)                       const_iterator;
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<iterator>)                     reverse_iterator;
   typedef BOOST_CONTAINER_IMPDEF(std::reverse_iterator<const_iterator>)               const_reverse_iterator;

   /// @cond

   private:                      // Internal typedefs
   BOOST_COPYABLE_AND_MOVABLE(deque)
   typedef typename Base::ptr_alloc_ptr index_pointer;
   static size_type s_buffer_size()
      { return Base::s_buffer_size(); }
   typedef allocator_traits<Allocator>                  allocator_traits_type;

   /// @endcond

   public:
   //////////////////////////////////////////////
   //
   //          construct/copy/destroy
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Default constructors a deque.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   deque()
      : Base()
   {}

   //! <b>Effects</b>: Constructs a deque taking the allocator as parameter.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   explicit deque(const allocator_type& a) BOOST_CONTAINER_NOEXCEPT
      : Base(a)
   {}

   //! <b>Effects</b>: Constructs a deque that will use a copy of allocator a
   //!   and inserts n value initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's default or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   explicit deque(size_type n)
      : Base(n, allocator_type())
   {
      container_detail::insert_value_initialized_n_proxy<Allocator, iterator> proxy(this->alloc());
      proxy.uninitialized_copy_n_and_update(this->begin(), n);
      //deque_base will deallocate in case of exception...
   }

   //! <b>Effects</b>: Constructs a deque that will use a copy of allocator a
   //!   and inserts n default initialized values.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's default or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   //!
   //! <b>Note</b>: Non-standard extension
   deque(size_type n, default_init_t)
      : Base(n, allocator_type())
   {
      container_detail::insert_default_initialized_n_proxy<Allocator, iterator> proxy(this->alloc());
      proxy.uninitialized_copy_n_and_update(this->begin(), n);
      //deque_base will deallocate in case of exception...
   }

   //! <b>Effects</b>: Constructs a deque that will use a copy of allocator a
   //!   and inserts n copies of value.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's default or copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   deque(size_type n, const value_type& value,
         const allocator_type& a = allocator_type())
      : Base(n, a)
   { this->priv_fill_initialize(value); }

   //! <b>Effects</b>: Constructs a deque that will use a copy of allocator a
   //!   and inserts a copy of the range [first, last) in the deque.
   //!
   //! <b>Throws</b>: If allocator_type's default constructor or copy constructor
   //!   throws or T's constructor taking an dereferenced InIt throws.
   //!
   //! <b>Complexity</b>: Linear to the range [first, last).
   template <class InIt>
   deque(InIt first, InIt last, const allocator_type& a = allocator_type()
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InIt, size_type>::value
         >::type * = 0
      #endif
      )
      : Base(a)
   {
      typedef typename std::iterator_traits<InIt>::iterator_category ItCat;
      this->priv_range_initialize(first, last, ItCat());
   }

   //! <b>Effects</b>: Copy constructs a deque.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   deque(const deque& x)
      :  Base(allocator_traits_type::select_on_container_copy_construction(x.alloc()))
   {
      if(x.size()){
         this->priv_initialize_map(x.size());
         boost::container::uninitialized_copy_alloc
            (this->alloc(), x.begin(), x.end(), this->members_.m_start);
      }
   }

   //! <b>Effects</b>: Move constructor. Moves mx's resources to *this.
   //!
   //! <b>Throws</b>: If allocator_type's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   deque(BOOST_RV_REF(deque) x)
      :  Base(boost::move(static_cast<Base&>(x)))
   {  this->swap_members(x);   }

   //! <b>Effects</b>: Copy constructs a vector using the specified allocator.
   //!
   //! <b>Postcondition</b>: x == *this.
   //!
   //! <b>Throws</b>: If allocation
   //!   throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the elements x contains.
   deque(const deque& x, const allocator_type &a)
      :  Base(a)
   {
      if(x.size()){
         this->priv_initialize_map(x.size());
         boost::container::uninitialized_copy_alloc
            (this->alloc(), x.begin(), x.end(), this->members_.m_start);
      }
   }

   //! <b>Effects</b>: Move constructor using the specified allocator.
   //!                 Moves mx's resources to *this if a == allocator_type().
   //!                 Otherwise copies values from x to *this.
   //!
   //! <b>Throws</b>: If allocation or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant if a == mx.get_allocator(), linear otherwise.
   deque(BOOST_RV_REF(deque) mx, const allocator_type &a)
      :  Base(a)
   {
      if(mx.alloc() == a){
         this->swap_members(mx);
      }
      else{
         if(mx.size()){
            this->priv_initialize_map(mx.size());
            boost::container::uninitialized_copy_alloc
               (this->alloc(), mx.begin(), mx.end(), this->members_.m_start);
         }
      }
   }

   //! <b>Effects</b>: Destroys the deque. All stored values are destroyed
   //!   and used memory is deallocated.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements.
   ~deque() BOOST_CONTAINER_NOEXCEPT
   {
      this->priv_destroy_range(this->members_.m_start, this->members_.m_finish);
   }

   //! <b>Effects</b>: Makes *this contain the same elements as x.
   //!
   //! <b>Postcondition</b>: this->size() == x.size(). *this contains a copy
   //! of each of x's elements.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in x.
   deque& operator= (BOOST_COPY_ASSIGN_REF(deque) x)
   {
      if (&x != this){
         allocator_type &this_alloc     = this->alloc();
         const allocator_type &x_alloc  = x.alloc();
         container_detail::bool_<allocator_traits_type::
            propagate_on_container_copy_assignment::value> flag;
         if(flag && this_alloc != x_alloc){
            this->clear();
            this->shrink_to_fit();
         }
         container_detail::assign_alloc(this->alloc(), x.alloc(), flag);
         container_detail::assign_alloc(this->ptr_alloc(), x.ptr_alloc(), flag);
         this->assign(x.cbegin(), x.cend());
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
   deque& operator= (BOOST_RV_REF(deque) x)
   {
      if (&x != this){
         allocator_type &this_alloc = this->alloc();
         allocator_type &x_alloc    = x.alloc();
         //If allocators are equal we can just swap pointers
         if(this_alloc == x_alloc){
            //Destroy objects but retain memory in case x reuses it in the future
            this->clear();
            this->swap_members(x);
            //Move allocator if needed
            container_detail::bool_<allocator_traits_type::
               propagate_on_container_move_assignment::value> flag;
            container_detail::move_alloc(this_alloc, x_alloc, flag);
            container_detail::move_alloc(this->ptr_alloc(), x.ptr_alloc(), flag);
         }
         //If unequal allocators, then do a one by one move
         else{
            this->assign( boost::make_move_iterator(x.begin())
                        , boost::make_move_iterator(x.end()));
         }
      }
      return *this;
   }

   //! <b>Effects</b>: Assigns the n copies of val to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   void assign(size_type n, const T& val)
   {
      typedef constant_iterator<value_type, difference_type> c_it;
      this->assign(c_it(val, n), c_it());
   }

   //! <b>Effects</b>: Assigns the the range [first, last) to *this.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's constructor from dereferencing InIt throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   template <class InIt>
   void assign(InIt first, InIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<InIt, size_type>::value
            && container_detail::is_input_iterator<InIt>::value
         >::type * = 0
      #endif
      )
   {
      iterator cur = this->begin();
      for ( ; first != last && cur != end(); ++cur, ++first){
         *cur = *first;
      }
      if (first == last){
         this->erase(cur, this->cend());
      }
      else{
         this->insert(this->cend(), first, last);
      }
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class FwdIt>
   void assign(FwdIt first, FwdIt last
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<FwdIt, size_type>::value
            && !container_detail::is_input_iterator<FwdIt>::value
         >::type * = 0
      )
   {
      const size_type len = std::distance(first, last);
      if (len > size()) {
         FwdIt mid = first;
         std::advance(mid, this->size());
         boost::container::copy(first, mid, begin());
         this->insert(this->cend(), mid, last);
      }
      else{
         this->erase(boost::container::copy(first, last, this->begin()), cend());
      }
   }
   #endif

   //! <b>Effects</b>: Returns a copy of the internal allocator.
   //!
   //! <b>Throws</b>: If allocator's copy constructor throws.
   //!
   //! <b>Complexity</b>: Constant.
   allocator_type get_allocator() const BOOST_CONTAINER_NOEXCEPT
   { return Base::alloc(); }

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   const stored_allocator_type &get_stored_allocator() const BOOST_CONTAINER_NOEXCEPT
   {  return Base::alloc(); }

   //////////////////////////////////////////////
   //
   //                iterators
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns a reference to the internal allocator.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension.
   stored_allocator_type &get_stored_allocator() BOOST_CONTAINER_NOEXCEPT
   {  return Base::alloc(); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator begin() BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_start; }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator begin() const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_start; }

   //! <b>Effects</b>: Returns an iterator to the end of the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   iterator end() BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_finish; }

   //! <b>Effects</b>: Returns a const_iterator to the end of the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator end() const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_finish; }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rbegin() BOOST_CONTAINER_NOEXCEPT
      { return reverse_iterator(this->members_.m_finish); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rbegin() const BOOST_CONTAINER_NOEXCEPT
      { return const_reverse_iterator(this->members_.m_finish); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reverse_iterator rend() BOOST_CONTAINER_NOEXCEPT
      { return reverse_iterator(this->members_.m_start); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator rend() const BOOST_CONTAINER_NOEXCEPT
      { return const_reverse_iterator(this->members_.m_start); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cbegin() const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_start; }

   //! <b>Effects</b>: Returns a const_iterator to the end of the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_iterator cend() const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_finish; }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crbegin() const BOOST_CONTAINER_NOEXCEPT
      { return const_reverse_iterator(this->members_.m_finish); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reverse_iterator crend() const BOOST_CONTAINER_NOEXCEPT
      { return const_reverse_iterator(this->members_.m_start); }

   //////////////////////////////////////////////
   //
   //                capacity
   //
   //////////////////////////////////////////////

   //! <b>Effects</b>: Returns true if the deque contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   bool empty() const BOOST_CONTAINER_NOEXCEPT
   { return this->members_.m_finish == this->members_.m_start; }

   //! <b>Effects</b>: Returns the number of the elements contained in the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type size() const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_finish - this->members_.m_start; }

   //! <b>Effects</b>: Returns the largest possible size of the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   size_type max_size() const BOOST_CONTAINER_NOEXCEPT
      { return allocator_traits_type::max_size(this->alloc()); }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are value initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size)
   {
      const size_type len = size();
      if (new_size < len)
         this->priv_erase_last_n(len - new_size);
      else{
         const size_type n = new_size - this->size();
         container_detail::insert_value_initialized_n_proxy<Allocator, iterator> proxy(this->alloc());
         priv_insert_back_aux_impl(n, proxy);
      }
   }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are default initialized.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   //!
   //! <b>Note</b>: Non-standard extension
   void resize(size_type new_size, default_init_t)
   {
      const size_type len = size();
      if (new_size < len)
         this->priv_erase_last_n(len - new_size);
      else{
         const size_type n = new_size - this->size();
         container_detail::insert_default_initialized_n_proxy<Allocator, iterator> proxy(this->alloc());
         priv_insert_back_aux_impl(n, proxy);
      }
   }

   //! <b>Effects</b>: Inserts or erases elements at the end such that
   //!   the size becomes n. New elements are copy constructed from x.
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to the difference between size() and new_size.
   void resize(size_type new_size, const value_type& x)
   {
      const size_type len = size();
      if (new_size < len)
         this->erase(this->members_.m_start + new_size, this->members_.m_finish);
      else
         this->insert(this->members_.m_finish, new_size - len, x);
   }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //!   with previous allocations. The size of the deque is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Constant.
   void shrink_to_fit()
   {
      //This deque implementation already
      //deallocates excess nodes when erasing
      //so there is nothing to do except for
      //empty deque
      if(this->empty()){
         this->priv_clear_map();
      }
   }

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
   reference front() BOOST_CONTAINER_NOEXCEPT
      { return *this->members_.m_start; }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the first element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference front() const BOOST_CONTAINER_NOEXCEPT
      { return *this->members_.m_start; }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference back() BOOST_CONTAINER_NOEXCEPT
      {  return *(end()-1); }

   //! <b>Requires</b>: !empty()
   //!
   //! <b>Effects</b>: Returns a const reference to the last
   //!   element of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference back() const BOOST_CONTAINER_NOEXCEPT
      {  return *(cend()-1);  }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   reference operator[](size_type n) BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_start[difference_type(n)]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   const_reference operator[](size_type n) const BOOST_CONTAINER_NOEXCEPT
      { return this->members_.m_start[difference_type(n)]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: std::range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   reference at(size_type n)
      { this->priv_range_check(n); return (*this)[n]; }

   //! <b>Requires</b>: size() > n.
   //!
   //! <b>Effects</b>: Returns a const reference to the nth element
   //!   from the beginning of the container.
   //!
   //! <b>Throws</b>: std::range_error if n >= size()
   //!
   //! <b>Complexity</b>: Constant.
   const_reference at(size_type n) const
      { this->priv_range_check(n); return (*this)[n]; }

   //////////////////////////////////////////////
   //
   //                modifiers
   //
   //////////////////////////////////////////////

   #if defined(BOOST_CONTAINER_PERFECT_FORWARDING) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the beginning of the deque.
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time
   template <class... Args>
   void emplace_front(Args&&... args)
   {
      if(this->priv_push_front_simple_available()){
         allocator_traits_type::construct
            ( this->alloc()
            , this->priv_push_front_simple_pos()
            , boost::forward<Args>(args)...);
         this->priv_push_front_simple_commit();
      }
      else{
         typedef container_detail::insert_non_movable_emplace_proxy<Allocator, iterator, Args...> type;
         this->priv_insert_front_aux_impl(1, type(this->alloc(), boost::forward<Args>(args)...));
      }
   }

   //! <b>Effects</b>: Inserts an object of type T constructed with
   //!   std::forward<Args>(args)... in the end of the deque.
   //!
   //! <b>Throws</b>: If memory allocation throws or the in-place constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time
   template <class... Args>
   void emplace_back(Args&&... args)
   {
      if(this->priv_push_back_simple_available()){
         allocator_traits_type::construct
            ( this->alloc()
            , this->priv_push_back_simple_pos()
            , boost::forward<Args>(args)...);
         this->priv_push_back_simple_commit();
      }
      else{
         typedef container_detail::insert_non_movable_emplace_proxy<Allocator, iterator, Args...> type;
         this->priv_insert_back_aux_impl(1, type(this->alloc(), boost::forward<Args>(args)...));
      }
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
   template <class... Args>
   iterator emplace(const_iterator p, Args&&... args)
   {
      if(p == this->cbegin()){
         this->emplace_front(boost::forward<Args>(args)...);
         return this->begin();
      }
      else if(p == this->cend()){
         this->emplace_back(boost::forward<Args>(args)...);
         return (this->end()-1);
      }
      else{
         typedef container_detail::insert_emplace_proxy<Allocator, iterator, Args...> type;
         return this->priv_insert_aux_impl(p, 1, type(this->alloc(), boost::forward<Args>(args)...));
      }
   }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   //advanced_insert_int.hpp includes all necessary preprocessor machinery...
   #define BOOST_PP_LOCAL_MACRO(n)                                                           \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >  )  \
   void emplace_front(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                    \
   {                                                                                         \
      if(priv_push_front_simple_available()){                                                \
         allocator_traits_type::construct                                                    \
            ( this->alloc()                                                                  \
            , this->priv_push_front_simple_pos()                                             \
              BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));               \
         priv_push_front_simple_commit();                                                    \
      }                                                                                      \
      else{                                                                                  \
         container_detail::BOOST_PP_CAT(insert_non_movable_emplace_proxy_arg, n)             \
               <Allocator, iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, P)> proxy               \
            (this->alloc() BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));  \
         priv_insert_front_aux_impl(1, proxy);                                               \
      }                                                                                      \
   }                                                                                         \
                                                                                             \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)    \
   void emplace_back(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                     \
   {                                                                                         \
      if(priv_push_back_simple_available()){                                                 \
         allocator_traits_type::construct                                                    \
            ( this->alloc()                                                                  \
            , this->priv_push_back_simple_pos()                                              \
              BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));               \
         priv_push_back_simple_commit();                                                     \
      }                                                                                      \
      else{                                                                                  \
         container_detail::BOOST_PP_CAT(insert_non_movable_emplace_proxy_arg, n)             \
               <Allocator, iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, P)> proxy               \
            (this->alloc() BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));  \
         priv_insert_back_aux_impl(1, proxy);                                                \
      }                                                                                      \
   }                                                                                         \
                                                                                             \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >)    \
   iterator emplace(const_iterator p                                                         \
                    BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))             \
   {                                                                                         \
      if(p == this->cbegin()){                                                               \
         this->emplace_front(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));         \
         return this->begin();                                                               \
      }                                                                                      \
      else if(p == cend()){                                                                  \
         this->emplace_back(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));          \
         return (this->end()-1);                                                             \
      }                                                                                      \
      else{                                                                                  \
         container_detail::BOOST_PP_CAT(insert_emplace_proxy_arg, n)                         \
            <Allocator, iterator BOOST_PP_ENUM_TRAILING_PARAMS(n, P)> proxy                  \
            (this->alloc() BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _));  \
         return this->priv_insert_aux_impl(p, 1, proxy);                                     \
      }                                                                                      \
   }                                                                                         \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x at the front of the deque.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_front(const T &x);

   //! <b>Effects</b>: Constructs a new element in the front of the deque
   //!   and moves the resources of mx to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_front(T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_front, T, void, priv_push_front)
   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Inserts a copy of x at the end of the deque.
   //!
   //! <b>Throws</b>: If memory allocation throws or
   //!   T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   void push_back(const T &x);

   //! <b>Effects</b>: Constructs a new element in the end of the deque
   //!   and moves the resources of mx to this new element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
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
   //! <b>Returns</b>: an iterator to the inserted element.
   //!
   //! <b>Throws</b>: If memory allocation throws or x's copy constructor throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, const T &x);

   //! <b>Requires</b>: position must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a new element before position with mx's resources.
   //!
   //! <b>Returns</b>: an iterator to the inserted element.
   //!
   //! <b>Throws</b>: If memory allocation throws.
   //!
   //! <b>Complexity</b>: If position is end(), amortized constant time
   //!   Linear time otherwise.
   iterator insert(const_iterator position, T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, T, iterator, priv_insert, const_iterator, const_iterator)
   #endif

   //! <b>Requires</b>: pos must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert n copies of x before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or pos if n is 0.
   //!
   //! <b>Throws</b>: If memory allocation throws or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to n.
   iterator insert(const_iterator pos, size_type n, const value_type& x)
   {
      typedef constant_iterator<value_type, difference_type> c_it;
      return this->insert(pos, c_it(x, n), c_it());
   }

   //! <b>Requires</b>: pos must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Insert a copy of the [first, last) range before pos.
   //!
   //! <b>Returns</b>: an iterator to the first inserted element or pos if first == last.
   //!
   //! <b>Throws</b>: If memory allocation throws, T's constructor from a
   //!   dereferenced InIt throws or T's copy constructor throws.
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
      size_type n = 0;
      iterator it(pos.unconst());
      for(;first != last; ++first, ++n){
         it = this->emplace(it, *first);
         ++it;
      }
      it -= n;
      return it;
   }

   #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class FwdIt>
   iterator insert(const_iterator p, FwdIt first, FwdIt last
      #if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
      , typename container_detail::enable_if_c
         < !container_detail::is_convertible<FwdIt, size_type>::value
            && !container_detail::is_input_iterator<FwdIt>::value
         >::type * = 0
      #endif
      )
   {
      container_detail::insert_range_proxy<Allocator, FwdIt, iterator> proxy(this->alloc(), first);
      return priv_insert_aux_impl(p, (size_type)std::distance(first, last), proxy);
   }
   #endif

   //! <b>Effects</b>: Removes the first element from the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   void pop_front() BOOST_CONTAINER_NOEXCEPT
   {
      if (this->members_.m_start.m_cur != this->members_.m_start.m_last - 1) {
         allocator_traits_type::destroy
            ( this->alloc()
            , container_detail::to_raw_pointer(this->members_.m_start.m_cur)
            );
         ++this->members_.m_start.m_cur;
      }
      else
         this->priv_pop_front_aux();
   }

   //! <b>Effects</b>: Removes the last element from the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   void pop_back() BOOST_CONTAINER_NOEXCEPT
   {
      if (this->members_.m_finish.m_cur != this->members_.m_finish.m_first) {
         --this->members_.m_finish.m_cur;
         allocator_traits_type::destroy
            ( this->alloc()
            , container_detail::to_raw_pointer(this->members_.m_finish.m_cur)
            );
      }
      else
         this->priv_pop_back_aux();
   }

   //! <b>Effects</b>: Erases the element at position pos.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements between pos and the
   //!   last element (if pos is near the end) or the first element
   //!   if(pos is near the beginning).
   //!   Constant if pos is the first or the last element.
   iterator erase(const_iterator pos) BOOST_CONTAINER_NOEXCEPT
   {
      iterator next = pos.unconst();
      ++next;
      size_type index = pos - this->members_.m_start;
      if (index < (this->size()/2)) {
         boost::move_backward(this->begin(), pos.unconst(), next);
         pop_front();
      }
      else {
         boost::move(next, this->end(), pos.unconst());
         pop_back();
      }
      return this->members_.m_start + index;
   }

   //! <b>Effects</b>: Erases the elements pointed by [first, last).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the distance between first and
   //!   last plus the elements between pos and the
   //!   last element (if pos is near the end) or the first element
   //!   if(pos is near the beginning).
   iterator erase(const_iterator first, const_iterator last) BOOST_CONTAINER_NOEXCEPT
   {
      if (first == this->members_.m_start && last == this->members_.m_finish) {
         this->clear();
         return this->members_.m_finish;
      }
      else {
         const size_type n = static_cast<size_type>(last - first);
         const size_type elems_before = static_cast<size_type>(first - this->members_.m_start);
         if (elems_before < (this->size() - n) - elems_before) {
            boost::move_backward(begin(), first.unconst(), last.unconst());
            iterator new_start = this->members_.m_start + n;
            if(!Base::traits_t::trivial_dctr_after_move)
               this->priv_destroy_range(this->members_.m_start, new_start);
            this->priv_destroy_nodes(this->members_.m_start.m_node, new_start.m_node);
            this->members_.m_start = new_start;
         }
         else {
            boost::move(last.unconst(), end(), first.unconst());
            iterator new_finish = this->members_.m_finish - n;
            if(!Base::traits_t::trivial_dctr_after_move)
               this->priv_destroy_range(new_finish, this->members_.m_finish);
            this->priv_destroy_nodes(new_finish.m_node + 1, this->members_.m_finish.m_node + 1);
            this->members_.m_finish = new_finish;
         }
         return this->members_.m_start + elems_before;
      }
   }

   //! <b>Effects</b>: Swaps the contents of *this and x.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   void swap(deque &x)
   {
      this->swap_members(x);
      container_detail::bool_<allocator_traits_type::propagate_on_container_swap::value> flag;
      container_detail::swap_alloc(this->alloc(), x.alloc(), flag);
      container_detail::swap_alloc(this->ptr_alloc(), x.ptr_alloc(), flag);
   }

   //! <b>Effects</b>: Erases all the elements of the deque.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the deque.
   void clear() BOOST_CONTAINER_NOEXCEPT
   {
      for (index_pointer node = this->members_.m_start.m_node + 1;
            node < this->members_.m_finish.m_node;
            ++node) {
         this->priv_destroy_range(*node, *node + this->s_buffer_size());
         this->priv_deallocate_node(*node);
      }

      if (this->members_.m_start.m_node != this->members_.m_finish.m_node) {
         this->priv_destroy_range(this->members_.m_start.m_cur, this->members_.m_start.m_last);
         this->priv_destroy_range(this->members_.m_finish.m_first, this->members_.m_finish.m_cur);
         this->priv_deallocate_node(this->members_.m_finish.m_first);
      }
      else
         this->priv_destroy_range(this->members_.m_start.m_cur, this->members_.m_finish.m_cur);

      this->members_.m_finish = this->members_.m_start;
   }

   /// @cond
   private:

   void priv_erase_last_n(size_type n)
   {
      if(n == this->size()) {
         this->clear();
      }
      else {
         iterator new_finish = this->members_.m_finish - n;
         if(!Base::traits_t::trivial_dctr_after_move)
            this->priv_destroy_range(new_finish, this->members_.m_finish);
         this->priv_destroy_nodes(new_finish.m_node + 1, this->members_.m_finish.m_node + 1);
         this->members_.m_finish = new_finish;
      }
   }

   void priv_range_check(size_type n) const
      {  if (n >= this->size())  throw_out_of_range("deque::at out of range");   }

   template <class U>
   iterator priv_insert(const_iterator position, BOOST_FWD_REF(U) x)
   {
      if (position == cbegin()){
         this->push_front(::boost::forward<U>(x));
         return begin();
      }
      else if (position == cend()){
         this->push_back(::boost::forward<U>(x));
         return --end();
      }
      else {
         return priv_insert_aux_impl
            (position, (size_type)1, container_detail::get_insert_value_proxy<iterator>(this->alloc(), ::boost::forward<U>(x)));
      }
   }

   template <class U>
   void priv_push_front(BOOST_FWD_REF(U) x)
   {
      if(this->priv_push_front_simple_available()){
         allocator_traits_type::construct
            ( this->alloc(), this->priv_push_front_simple_pos(), ::boost::forward<U>(x));
         this->priv_push_front_simple_commit();
      }
      else{
         priv_insert_aux_impl
            (this->cbegin(), (size_type)1, container_detail::get_insert_value_proxy<iterator>(this->alloc(), ::boost::forward<U>(x)));
      }
   }

   template <class U>
   void priv_push_back(BOOST_FWD_REF(U) x)
   {
      if(this->priv_push_back_simple_available()){
         allocator_traits_type::construct
            ( this->alloc(), this->priv_push_back_simple_pos(), ::boost::forward<U>(x));
         this->priv_push_back_simple_commit();
      }
      else{
         priv_insert_aux_impl
            (this->cend(), (size_type)1, container_detail::get_insert_value_proxy<iterator>(this->alloc(), ::boost::forward<U>(x)));
         container_detail::insert_copy_proxy<Allocator, iterator> proxy(this->alloc(), x);
      }
   }

   bool priv_push_back_simple_available() const
   {
      return this->members_.m_map &&
         (this->members_.m_finish.m_cur != (this->members_.m_finish.m_last - 1));
   }

   T *priv_push_back_simple_pos() const
   {
      return container_detail::to_raw_pointer(this->members_.m_finish.m_cur);
   }

   void priv_push_back_simple_commit()
   {
      ++this->members_.m_finish.m_cur;
   }

   bool priv_push_front_simple_available() const
   {
      return this->members_.m_map &&
         (this->members_.m_start.m_cur != this->members_.m_start.m_first);
   }

   T *priv_push_front_simple_pos() const
   {  return container_detail::to_raw_pointer(this->members_.m_start.m_cur) - 1;  }

   void priv_push_front_simple_commit()
   {  --this->members_.m_start.m_cur;   }

   void priv_destroy_range(iterator p, iterator p2)
   {
      for(;p != p2; ++p){
         allocator_traits_type::destroy
            ( this->alloc()
            , container_detail::to_raw_pointer(&*p)
            );
      }
   }

   void priv_destroy_range(pointer p, pointer p2)
   {
      for(;p != p2; ++p){
         allocator_traits_type::destroy
            ( this->alloc()
            , container_detail::to_raw_pointer(&*p)
            );
      }
   }

   template<class InsertProxy>
   iterator priv_insert_aux_impl(const_iterator p, size_type n, InsertProxy interf)
   {
      iterator pos(p.unconst());
      const size_type pos_n = p - this->cbegin();
      if(!this->members_.m_map){
         this->priv_initialize_map(0);
         pos = this->begin();
      }

      const size_type elemsbefore = static_cast<size_type>(pos - this->members_.m_start);
      const size_type length = this->size();
      if (elemsbefore < length / 2) {
         const iterator new_start = this->priv_reserve_elements_at_front(n);
         const iterator old_start = this->members_.m_start;
         if(!elemsbefore){
            interf.uninitialized_copy_n_and_update(new_start, n);
            this->members_.m_start = new_start;
         }
         else{
            pos = this->members_.m_start + elemsbefore;
            if (elemsbefore >= n) {
               const iterator start_n = this->members_.m_start + n;
               ::boost::container::uninitialized_move_alloc
                  (this->alloc(), this->members_.m_start, start_n, new_start);
               this->members_.m_start = new_start;
               boost::move(start_n, pos, old_start);
               interf.copy_n_and_update(pos - n, n);
            }
            else {
               const size_type mid_count = n - elemsbefore;
               const iterator mid_start = old_start - mid_count;
               interf.uninitialized_copy_n_and_update(mid_start, mid_count);
               this->members_.m_start = mid_start;
               ::boost::container::uninitialized_move_alloc
                  (this->alloc(), old_start, pos, new_start);
               this->members_.m_start = new_start;
               interf.copy_n_and_update(old_start, elemsbefore);
            }
         }
      }
      else {
         const iterator new_finish = this->priv_reserve_elements_at_back(n);
         const iterator old_finish = this->members_.m_finish;
         const size_type elemsafter = length - elemsbefore;
         if(!elemsafter){
            interf.uninitialized_copy_n_and_update(old_finish, n);
            this->members_.m_finish = new_finish;
         }
         else{
            pos = old_finish - elemsafter;
            if (elemsafter >= n) {
               iterator finish_n = old_finish - difference_type(n);
               ::boost::container::uninitialized_move_alloc
                  (this->alloc(), finish_n, old_finish, old_finish);
               this->members_.m_finish = new_finish;
               boost::move_backward(pos, finish_n, old_finish);
               interf.copy_n_and_update(pos, n);
            }
            else {
               const size_type raw_gap = n - elemsafter;
               ::boost::container::uninitialized_move_alloc
                  (this->alloc(), pos, old_finish, old_finish + raw_gap);
               BOOST_TRY{
                  interf.copy_n_and_update(pos, elemsafter);
                  interf.uninitialized_copy_n_and_update(old_finish, raw_gap);
               }
               BOOST_CATCH(...){
                  this->priv_destroy_range(old_finish, old_finish + elemsafter);
                  BOOST_RETHROW
               }
               BOOST_CATCH_END
               this->members_.m_finish = new_finish;
            }
         }
      }
      return this->begin() + pos_n;
   }

   template <class InsertProxy>
   iterator priv_insert_back_aux_impl(size_type n, InsertProxy interf)
   {
      if(!this->members_.m_map){
         this->priv_initialize_map(0);
      }

      iterator new_finish = this->priv_reserve_elements_at_back(n);
      iterator old_finish = this->members_.m_finish;
      interf.uninitialized_copy_n_and_update(old_finish, n);
      this->members_.m_finish = new_finish;
      return iterator(this->members_.m_finish - n);
   }

   template <class InsertProxy>
   iterator priv_insert_front_aux_impl(size_type n, InsertProxy interf)
   {
      if(!this->members_.m_map){
         this->priv_initialize_map(0);
      }

      iterator new_start = this->priv_reserve_elements_at_front(n);
      interf.uninitialized_copy_n_and_update(new_start, n);
      this->members_.m_start = new_start;
      return new_start;
   }

   iterator priv_fill_insert(const_iterator pos, size_type n, const value_type& x)
   {
      typedef constant_iterator<value_type, difference_type> c_it;
      return this->insert(pos, c_it(x, n), c_it());
   }

   // Precondition: this->members_.m_start and this->members_.m_finish have already been initialized,
   // but none of the deque's elements have yet been constructed.
   void priv_fill_initialize(const value_type& value)
   {
      index_pointer cur;
      BOOST_TRY {
         for (cur = this->members_.m_start.m_node; cur < this->members_.m_finish.m_node; ++cur){
            boost::container::uninitialized_fill_alloc
               (this->alloc(), *cur, *cur + this->s_buffer_size(), value);
         }
         boost::container::uninitialized_fill_alloc
            (this->alloc(), this->members_.m_finish.m_first, this->members_.m_finish.m_cur, value);
      }
      BOOST_CATCH(...){
         this->priv_destroy_range(this->members_.m_start, iterator(*cur, cur));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   template <class InIt>
   void priv_range_initialize(InIt first, InIt last, std::input_iterator_tag)
   {
      this->priv_initialize_map(0);
      BOOST_TRY {
         for ( ; first != last; ++first)
            this->emplace_back(*first);
      }
      BOOST_CATCH(...){
         this->clear();
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   template <class FwdIt>
   void priv_range_initialize(FwdIt first, FwdIt last, std::forward_iterator_tag)
   {
      size_type n = 0;
      n = std::distance(first, last);
      this->priv_initialize_map(n);

      index_pointer cur_node;
      BOOST_TRY {
         for (cur_node = this->members_.m_start.m_node;
               cur_node < this->members_.m_finish.m_node;
               ++cur_node) {
            FwdIt mid = first;
            std::advance(mid, this->s_buffer_size());
            ::boost::container::uninitialized_copy_alloc(this->alloc(), first, mid, *cur_node);
            first = mid;
         }
         ::boost::container::uninitialized_copy_alloc(this->alloc(), first, last, this->members_.m_finish.m_first);
      }
      BOOST_CATCH(...){
         this->priv_destroy_range(this->members_.m_start, iterator(*cur_node, cur_node));
         BOOST_RETHROW
      }
      BOOST_CATCH_END
   }

   // Called only if this->members_.m_finish.m_cur == this->members_.m_finish.m_first.
   void priv_pop_back_aux() BOOST_CONTAINER_NOEXCEPT
   {
      this->priv_deallocate_node(this->members_.m_finish.m_first);
      this->members_.m_finish.priv_set_node(this->members_.m_finish.m_node - 1);
      this->members_.m_finish.m_cur = this->members_.m_finish.m_last - 1;
      allocator_traits_type::destroy
         ( this->alloc()
         , container_detail::to_raw_pointer(this->members_.m_finish.m_cur)
         );
   }

   // Called only if this->members_.m_start.m_cur == this->members_.m_start.m_last - 1.  Note that
   // if the deque has at least one element (a precondition for this member
   // function), and if this->members_.m_start.m_cur == this->members_.m_start.m_last, then the deque
   // must have at least two nodes.
   void priv_pop_front_aux() BOOST_CONTAINER_NOEXCEPT
   {
      allocator_traits_type::destroy
         ( this->alloc()
         , container_detail::to_raw_pointer(this->members_.m_start.m_cur)
         );
      this->priv_deallocate_node(this->members_.m_start.m_first);
      this->members_.m_start.priv_set_node(this->members_.m_start.m_node + 1);
      this->members_.m_start.m_cur = this->members_.m_start.m_first;
   }     

   iterator priv_reserve_elements_at_front(size_type n)
   {
      size_type vacancies = this->members_.m_start.m_cur - this->members_.m_start.m_first;
      if (n > vacancies){
         size_type new_elems = n-vacancies;
         size_type new_nodes = (new_elems + this->s_buffer_size() - 1) /
            this->s_buffer_size();
         size_type s = (size_type)(this->members_.m_start.m_node - this->members_.m_map);
         if (new_nodes > s){
            this->priv_reallocate_map(new_nodes, true);
         }
         size_type i = 1;
         BOOST_TRY {
            for (; i <= new_nodes; ++i)
               *(this->members_.m_start.m_node - i) = this->priv_allocate_node();
         }
         BOOST_CATCH(...) {
            for (size_type j = 1; j < i; ++j)
               this->priv_deallocate_node(*(this->members_.m_start.m_node - j));     
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
      return this->members_.m_start - difference_type(n);
   }

   iterator priv_reserve_elements_at_back(size_type n)
   {
      size_type vacancies = (this->members_.m_finish.m_last - this->members_.m_finish.m_cur) - 1;
      if (n > vacancies){
         size_type new_elems = n - vacancies;
         size_type new_nodes = (new_elems + this->s_buffer_size() - 1)/s_buffer_size();
         size_type s = (size_type)(this->members_.m_map_size - (this->members_.m_finish.m_node - this->members_.m_map));
         if (new_nodes + 1 > s){
            this->priv_reallocate_map(new_nodes, false);
         }
         size_type i;
         BOOST_TRY {
            for (i = 1; i <= new_nodes; ++i)
               *(this->members_.m_finish.m_node + i) = this->priv_allocate_node();
         }
         BOOST_CATCH(...) {
            for (size_type j = 1; j < i; ++j)
               this->priv_deallocate_node(*(this->members_.m_finish.m_node + j));     
            BOOST_RETHROW
         }
         BOOST_CATCH_END
      }
      return this->members_.m_finish + difference_type(n);
   }

   void priv_reallocate_map(size_type nodes_to_add, bool add_at_front)
   {
      size_type old_num_nodes = this->members_.m_finish.m_node - this->members_.m_start.m_node + 1;
      size_type new_num_nodes = old_num_nodes + nodes_to_add;

      index_pointer new_nstart;
      if (this->members_.m_map_size > 2 * new_num_nodes) {
         new_nstart = this->members_.m_map + (this->members_.m_map_size - new_num_nodes) / 2
                           + (add_at_front ? nodes_to_add : 0);
         if (new_nstart < this->members_.m_start.m_node)
            boost::move(this->members_.m_start.m_node, this->members_.m_finish.m_node + 1, new_nstart);
         else
            boost::move_backward
               (this->members_.m_start.m_node, this->members_.m_finish.m_node + 1, new_nstart + old_num_nodes);
      }
      else {
         size_type new_map_size =
            this->members_.m_map_size + container_detail::max_value(this->members_.m_map_size, nodes_to_add) + 2;

         index_pointer new_map = this->priv_allocate_map(new_map_size);
         new_nstart = new_map + (new_map_size - new_num_nodes) / 2
                              + (add_at_front ? nodes_to_add : 0);
         boost::move(this->members_.m_start.m_node, this->members_.m_finish.m_node + 1, new_nstart);
         this->priv_deallocate_map(this->members_.m_map, this->members_.m_map_size);

         this->members_.m_map = new_map;
         this->members_.m_map_size = new_map_size;
      }

      this->members_.m_start.priv_set_node(new_nstart);
      this->members_.m_finish.priv_set_node(new_nstart + old_num_nodes - 1);
   }
   /// @endcond
};

// Nonmember functions.
template <class T, class Allocator>
inline bool operator==(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
{
   return x.size() == y.size() && equal(x.begin(), x.end(), y.begin());
}

template <class T, class Allocator>
inline bool operator<(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
{
   return lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());
}

template <class T, class Allocator>
inline bool operator!=(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
   {  return !(x == y);   }

template <class T, class Allocator>
inline bool operator>(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
   {  return y < x; }

template <class T, class Allocator>
inline bool operator>=(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
   {  return !(x < y); }

template <class T, class Allocator>
inline bool operator<=(const deque<T, Allocator>& x, const deque<T, Allocator>& y) BOOST_CONTAINER_NOEXCEPT
   {  return !(y < x); }

template <class T, class Allocator>
inline void swap(deque<T, Allocator>& x, deque<T, Allocator>& y)
{  x.swap(y);  }

}}

/// @cond

namespace boost {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator>
struct has_trivial_destructor_after_move<boost::container::deque<T, Allocator> >
   : public ::boost::has_trivial_destructor_after_move<Allocator>
{};

}

/// @endcond

#include <boost/container/detail/config_end.hpp>

#endif //   #ifndef  BOOST_CONTAINER_DEQUE_HPP
