//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_MULTIALLOCATION_CHAIN_HPP
#define BOOST_CONTAINER_DETAIL_MULTIALLOCATION_CHAIN_HPP

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
// container/detail
#include <boost/move/detail/to_raw_pointer.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/placement_new.hpp>
#include <boost/container/detail/iterator.hpp>
// intrusive
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/twin.hpp>
// move
#include <boost/move/utility_core.hpp>
#include <boost/move/detail/iterator_to_raw_pointer.hpp>


namespace boost {
namespace container {
namespace dtl {

template<class VoidPointer>
class basic_multiallocation_chain
{
   private:
   typedef bi::slist_base_hook<bi::void_pointer<VoidPointer>
                        ,bi::link_mode<bi::normal_link>
                        > node;

   typedef typename boost::intrusive::pointer_traits
      <VoidPointer>::template rebind_pointer<char>::type    char_ptr;
   typedef typename boost::intrusive::
      pointer_traits<char_ptr>::difference_type             difference_type;

   typedef bi::slist< node
                    , bi::linear<true>
                    , bi::cache_last<true>
                    , bi::size_type<typename boost::container::dtl::make_unsigned<difference_type>::type>
                    > slist_impl_t;
   slist_impl_t slist_impl_;

   typedef typename boost::intrusive::pointer_traits
      <VoidPointer>::template rebind_pointer<node>::type    node_ptr;
   typedef typename boost::intrusive::
      pointer_traits<node_ptr>                              node_ptr_traits;

   static node & to_node(const VoidPointer &p)
   {  return *static_cast<node*>(static_cast<void*>(boost::movelib::to_raw_pointer(p)));  }

   static VoidPointer from_node(node &n)
   {  return node_ptr_traits::pointer_to(n);  }

   static node_ptr to_node_ptr(const VoidPointer &p)
   {  return node_ptr_traits::static_cast_from(p);   }

   BOOST_MOVABLE_BUT_NOT_COPYABLE(basic_multiallocation_chain)

   public:

   typedef VoidPointer                       void_pointer;
   typedef typename slist_impl_t::iterator   iterator;
   typedef typename slist_impl_t::size_type  size_type;
   typedef boost::intrusive::twin<void_pointer> pointer_pair;

   basic_multiallocation_chain()
      :  slist_impl_()
   {}

   basic_multiallocation_chain(const void_pointer &b, const void_pointer &before_e, size_type n)
      :  slist_impl_(to_node_ptr(b), to_node_ptr(before_e), n)
   {}

   basic_multiallocation_chain(BOOST_RV_REF(basic_multiallocation_chain) other)
      :  slist_impl_(::boost::move(other.slist_impl_))
   {}

   basic_multiallocation_chain& operator=(BOOST_RV_REF(basic_multiallocation_chain) other)
   {
      slist_impl_ = ::boost::move(other.slist_impl_);
      return *this;
   }

   bool empty() const
   {  return slist_impl_.empty(); }

   size_type size() const
   {  return slist_impl_.size();  }

   iterator before_begin()
   {  return slist_impl_.before_begin(); }

   iterator begin()
   {  return slist_impl_.begin(); }

   iterator end()
   {  return slist_impl_.end(); }

   iterator last()
   {  return slist_impl_.last(); }

   void clear()
   {  slist_impl_.clear(); }

   iterator insert_after(iterator it, void_pointer m)
   {  return slist_impl_.insert_after(it, to_node(m));   }

   void push_front(const void_pointer &m)
   {  return slist_impl_.push_front(to_node(m));  }

   void push_back(const void_pointer &m)
   {  return slist_impl_.push_back(to_node(m));   }

   void_pointer pop_front()
   {
      node & n = slist_impl_.front();
      void_pointer ret = from_node(n);
      slist_impl_.pop_front();
      return ret;
   }

   void splice_after(iterator after_this, basic_multiallocation_chain &x, iterator before_b, iterator before_e, size_type n)
   {  slist_impl_.splice_after(after_this, x.slist_impl_, before_b, before_e, n);   }

   void splice_after(iterator after_this, basic_multiallocation_chain &x)
   {  slist_impl_.splice_after(after_this, x.slist_impl_);   }

   void erase_after(iterator before_b, iterator e, size_type n)
   {  slist_impl_.erase_after(before_b, e, n);   }

   void_pointer incorporate_after(iterator after_this, const void_pointer &b, size_type unit_bytes, size_type num_units)
   {
      typedef typename boost::intrusive::pointer_traits<char_ptr> char_pointer_traits;
      char_ptr elem = char_pointer_traits::static_cast_from(b);
      if(num_units){
         char_ptr prev_elem = elem;
         elem += difference_type(unit_bytes);
         for(size_type i = 0; i != num_units-1u; ++i, elem += difference_type(unit_bytes)){
            ::new (boost::movelib::to_raw_pointer(prev_elem), boost_container_new_t()) void_pointer(elem);
            prev_elem = elem;
         }
         slist_impl_.incorporate_after(after_this, to_node_ptr(b), to_node_ptr(prev_elem), num_units);
      }
      return elem;
   }

   void incorporate_after(iterator after_this, void_pointer b, void_pointer before_e, size_type n)
   {  slist_impl_.incorporate_after(after_this, to_node_ptr(b), to_node_ptr(before_e), n);   }

   void swap(basic_multiallocation_chain &x)
   {  slist_impl_.swap(x.slist_impl_);   }

   static iterator iterator_to(const void_pointer &p)
   {  return slist_impl_t::s_iterator_to(to_node(p));   }

   pointer_pair extract_data()
   {
      if(BOOST_LIKELY(!slist_impl_.empty())){
         pointer_pair ret
            (slist_impl_.begin().operator->()
            ,slist_impl_.last().operator->());
         slist_impl_.clear();
         return ret;
      }
      else {
         return pointer_pair();
      }
   }
};

template <class Iterator, class T>
class multialloc_iterator
   : public boost::container::iterator
      < typename Iterator::iterator_category
      , T
      , typename Iterator::difference_type
      , T*
      , T&
      >
{
   public:
   inline explicit multialloc_iterator(const Iterator &it)
      :  m_it(it)
   {}

   inline explicit multialloc_iterator()
      :  m_it()
   {}

   //Constructors
   inline multialloc_iterator& operator++()
   { increment();   return *this;   }

      inline multialloc_iterator operator++(int)
   {
      multialloc_iterator result (*this);
      increment();
      return result;
   }

   inline friend bool operator== (const multialloc_iterator& i, const multialloc_iterator& i2)
   { return i.equal(i2); }

   inline friend bool operator!= (const multialloc_iterator& i, const multialloc_iterator& i2)
   { return !(i == i2); }

   inline friend typename Iterator::difference_type operator- (const multialloc_iterator& i, const multialloc_iterator& i2)
   { return i2.distance_to(i); }

   //Arithmetic
   inline multialloc_iterator& operator+=(typename Iterator::difference_type off)
   {  this->advance(off); return *this;   }

   inline multialloc_iterator operator+(typename Iterator::difference_type off) const
   {
      multialloc_iterator other(*this);
      other.advance(off);
      return other;
   }

   inline friend multialloc_iterator operator+(typename Iterator::difference_type off, const multialloc_iterator& right)
   {  return right + off; }

   inline multialloc_iterator& operator-=(typename Iterator::difference_type off)
   {  this->advance(-off); return *this;   }

   inline multialloc_iterator operator-(typename Iterator::difference_type off) const
   {  return *this + (-off);  }

   inline T& operator*() const
   { return *this->operator->(); }

   inline T* operator->() const
   {  return static_cast<T*>(static_cast<void*>(boost::movelib::iterator_to_raw_pointer(m_it)));   }

   inline Iterator & base()
   {  return m_it;   }

   inline const Iterator & base() const
   {  return m_it;   }

   private:
   Iterator m_it;

   inline void increment()
   { ++m_it; }

   inline void decrement()
   { --m_it; }

   inline bool equal(const multialloc_iterator &other) const
   {  return m_it == other.m_it;   }

   inline bool less(const multialloc_iterator &other) const
   {  return other.m_it < m_it;   }

   inline void advance(typename Iterator::difference_type n)
   {  boost::container::iterator_advance(m_it, n); }

   inline typename Iterator::difference_type distance_to(const multialloc_iterator &other)const
   {  return boost::container::iterator_distance(other.m_it, m_it); }
};


template<class MultiallocationChain, class T>
class transform_multiallocation_chain
   : public MultiallocationChain
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(transform_multiallocation_chain)
   //transform_multiallocation_chain(const transform_multiallocation_chain &);
   //transform_multiallocation_chain & operator=(const transform_multiallocation_chain &);

   typedef typename MultiallocationChain::void_pointer   void_pointer;
   typedef typename boost::intrusive::pointer_traits
      <void_pointer>                                     void_pointer_traits;
   typedef typename void_pointer_traits::template
      rebind_pointer<T>::type                            pointer;
   typedef typename boost::intrusive::pointer_traits
      <pointer>                                          pointer_traits;

   static pointer cast(const void_pointer &p)
   {  return pointer_traits::static_cast_from(p);  }

   public:
   typedef multialloc_iterator
      <typename MultiallocationChain::iterator, T>       iterator;
   typedef typename MultiallocationChain::size_type      size_type;
   typedef boost::intrusive::twin<pointer>               pointer_pair;

   transform_multiallocation_chain()
      : MultiallocationChain()
   {}

   transform_multiallocation_chain(BOOST_RV_REF(transform_multiallocation_chain) other)
      : MultiallocationChain(::boost::move(static_cast<MultiallocationChain&>(other)))
   {}

   transform_multiallocation_chain(BOOST_RV_REF(MultiallocationChain) other)
      : MultiallocationChain(::boost::move(static_cast<MultiallocationChain&>(other)))
   {}

   transform_multiallocation_chain& operator=(BOOST_RV_REF(transform_multiallocation_chain) other)
   {
      return static_cast<MultiallocationChain&>
         (this->MultiallocationChain::operator=(::boost::move(static_cast<MultiallocationChain&>(other))));
   }

   void push_front(const pointer &mem)
   {   this->MultiallocationChain::push_front(mem);  }

   void push_back(const pointer &mem)
   {  return this->MultiallocationChain::push_back(mem);   }

   void swap(transform_multiallocation_chain &other_chain)
   {  this->MultiallocationChain::swap(other_chain); }

   void splice_after(iterator after_this, transform_multiallocation_chain &x, iterator before_b, iterator before_e, size_type n)
   {  this->MultiallocationChain::splice_after(after_this.base(), x, before_b.base(), before_e.base(), n);  }

   void incorporate_after(iterator after_this, pointer b, pointer before_e, size_type n)
   {  this->MultiallocationChain::incorporate_after(after_this.base(), b, before_e, n);  }

   pointer pop_front()
   {  return cast(this->MultiallocationChain::pop_front());  }

   bool empty() const
   {  return this->MultiallocationChain::empty(); }

   iterator before_begin()
   {  return iterator(this->MultiallocationChain::before_begin());   }

   iterator begin()
   {  return iterator(this->MultiallocationChain::begin());   }

   iterator last()
   {  return iterator(this->MultiallocationChain::last());  }

   iterator end()
   {  return iterator(this->MultiallocationChain::end());   }

   size_type size() const
   {  return this->MultiallocationChain::size();  }

   void clear()
   {  this->MultiallocationChain::clear(); }

   iterator insert_after(iterator it, pointer m)
   {  return iterator(this->MultiallocationChain::insert_after(it.base(), m)); }

   static iterator iterator_to(const pointer &p)
   {  return iterator(MultiallocationChain::iterator_to(p));  }

   pointer_pair extract_data()
   {
      typename MultiallocationChain::pointer_pair data(this->MultiallocationChain::extract_data());
      return pointer_pair(cast(data.first), cast(data.second));
   }
/*
   MultiallocationChain &extract_multiallocation_chain()
   {  return holder_;  }*/
};

}}}

// namespace dtl {
// namespace container {
// namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //BOOST_CONTAINER_DETAIL_MULTIALLOCATION_CHAIN_HPP
