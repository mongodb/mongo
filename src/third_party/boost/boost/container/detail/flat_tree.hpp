////////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_FLAT_TREE_HPP
#define BOOST_CONTAINER_FLAT_TREE_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include "config_begin.hpp"
#include <boost/container/detail/workaround.hpp>

#include <boost/container/container_fwd.hpp>

#include <algorithm>
#include <functional>
#include <utility>

#include <boost/type_traits/has_trivial_destructor.hpp>
#include <boost/move/move.hpp>

#include <boost/container/detail/utilities.hpp>
#include <boost/container/detail/pair.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/detail/value_init.hpp>
#include <boost/container/detail/destroyers.hpp>

namespace boost {

namespace container {

namespace container_detail {

template<class Compare, class Value, class KeyOfValue>
class flat_tree_value_compare
   : private Compare
{
   typedef Value              first_argument_type;
   typedef Value              second_argument_type;
   typedef bool               return_type;
   public:     
   flat_tree_value_compare()
      : Compare()
   {}

   flat_tree_value_compare(const Compare &pred) 
      : Compare(pred)
   {}

   bool operator()(const Value& lhs, const Value& rhs) const
   { 
      KeyOfValue key_extract;
      return Compare::operator()(key_extract(lhs), key_extract(rhs)); 
   }

   const Compare &get_comp() const
      {  return *this;  }
   
   Compare &get_comp()
      {  return *this;  }
};

template<class Pointer>
struct get_flat_tree_iterators
{
   typedef typename container_detail::
      vector_iterator<Pointer>                        iterator;
   typedef typename container_detail::
      vector_const_iterator<Pointer>                  const_iterator;
   typedef std::reverse_iterator<iterator>            reverse_iterator;
   typedef std::reverse_iterator<const_iterator>      const_reverse_iterator;
};

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
class flat_tree
{
   typedef boost::container::vector<Value, A>  vector_t;
   typedef A                                   allocator_t;

   public:
   typedef flat_tree_value_compare<Compare, Value, KeyOfValue> value_compare;

 private:
   struct Data 
      //Inherit from value_compare to do EBO
      : public value_compare
   {
      BOOST_COPYABLE_AND_MOVABLE(Data)

      public:
      Data()
         : value_compare(), m_vect()
      {}

      Data(const Data &d)
         : value_compare(d), m_vect(d.m_vect)
      {}

      Data(BOOST_RV_REF(Data) d)
         : value_compare(boost::move(d)), m_vect(boost::move(d.m_vect))
      {}

      Data(const Compare &comp) 
         : value_compare(comp), m_vect()
      {}

      Data(const Compare &comp,
           const allocator_t &alloc) 
         : value_compare(comp), m_vect(alloc)
      {}

      Data& operator=(BOOST_COPY_ASSIGN_REF(Data) d)
      {
         this->value_compare::operator=(d);
         m_vect = d.m_vect;
         return *this;
      }

      Data& operator=(BOOST_RV_REF(Data) d)
      {
         this->value_compare::operator=(boost::move(static_cast<value_compare &>(d)));
         m_vect = boost::move(d.m_vect);
         return *this;
      }

      void swap(Data &d)
      {
         value_compare& mycomp    = *this, & othercomp = d;
         container_detail::do_swap(mycomp, othercomp);
         this->m_vect.swap(d.m_vect);
      }

      vector_t m_vect;
   };

   Data m_data;
   BOOST_COPYABLE_AND_MOVABLE(flat_tree)

   public:

   typedef typename vector_t::value_type              value_type;
   typedef typename vector_t::pointer                 pointer;
   typedef typename vector_t::const_pointer           const_pointer;
   typedef typename vector_t::reference               reference;
   typedef typename vector_t::const_reference         const_reference;
   typedef Key                                        key_type;
   typedef Compare                                    key_compare;
   typedef typename vector_t::allocator_type          allocator_type;
   typedef typename vector_t::size_type               size_type;
   typedef typename vector_t::difference_type         difference_type;
   typedef typename vector_t::iterator                iterator;
   typedef typename vector_t::const_iterator          const_iterator;
   typedef typename vector_t::reverse_iterator        reverse_iterator;
   typedef typename vector_t::const_reverse_iterator  const_reverse_iterator;

   //!Standard extension
   typedef allocator_type                             stored_allocator_type;

   flat_tree()
      : m_data()
   { }

   explicit flat_tree(const Compare& comp)
      : m_data(comp)
   { }

   flat_tree(const Compare& comp, const allocator_type& a)
      : m_data(comp, a)
   { }

   flat_tree(const flat_tree& x) 
      :  m_data(x.m_data)
   { }

   flat_tree(BOOST_RV_REF(flat_tree) x)
      :  m_data(boost::move(x.m_data))
   { }

   template <class InputIterator>
   flat_tree( ordered_range_t, InputIterator first, InputIterator last
            , const Compare& comp     = Compare()
            , const allocator_type& a = allocator_type())
      : m_data(comp, a)
   { this->m_data.m_vect.insert(this->m_data.m_vect.end(), first, last); }

   ~flat_tree()
   { }

   flat_tree&  operator=(BOOST_COPY_ASSIGN_REF(flat_tree) x)
   {  m_data = x.m_data;   return *this;  }

   flat_tree&  operator=(BOOST_RV_REF(flat_tree) mx)
   {  m_data = boost::move(mx.m_data); return *this;  }

   public:    
   // accessors:
   Compare key_comp() const 
   { return this->m_data.get_comp(); }

   allocator_type get_allocator() const 
   { return this->m_data.m_vect.get_allocator(); }

   const stored_allocator_type &get_stored_allocator() const 
   {  return this->m_data.m_vect.get_stored_allocator(); }

   stored_allocator_type &get_stored_allocator()
   {  return this->m_data.m_vect.get_stored_allocator(); }

   iterator begin() 
   { return this->m_data.m_vect.begin(); }

   const_iterator begin() const 
   { return this->cbegin(); }

   const_iterator cbegin() const 
   { return this->m_data.m_vect.begin(); }

   iterator end() 
   { return this->m_data.m_vect.end(); }

   const_iterator end() const 
   { return this->cend(); }

   const_iterator cend() const 
   { return this->m_data.m_vect.end(); }

   reverse_iterator rbegin() 
   { return reverse_iterator(this->end()); }

   const_reverse_iterator rbegin() const 
   {  return this->crbegin();  }

   const_reverse_iterator crbegin() const 
   {  return const_reverse_iterator(this->cend());  }

   reverse_iterator rend() 
   { return reverse_iterator(this->begin()); }

   const_reverse_iterator rend() const 
   { return this->crend(); } 

   const_reverse_iterator crend() const 
   { return const_reverse_iterator(this->cbegin()); } 

   bool empty() const 
   { return this->m_data.m_vect.empty(); }

   size_type size() const 
   { return this->m_data.m_vect.size(); }

   size_type max_size() const 
   { return this->m_data.m_vect.max_size(); }

   void swap(flat_tree& other) 
   {  this->m_data.swap(other.m_data);  }

   public:
   // insert/erase
   std::pair<iterator,bool> insert_unique(const value_type& val)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(val, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, val);
      }
      return ret;
   }

   std::pair<iterator,bool> insert_unique(BOOST_RV_REF(value_type) val)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(val, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, boost::move(val));
      }
      return ret;
   }


   iterator insert_equal(const value_type& val)
   {
      iterator i = this->upper_bound(KeyOfValue()(val));
      i = this->m_data.m_vect.insert(i, val);
      return i;
   }

   iterator insert_equal(BOOST_RV_REF(value_type) mval)
   {
      iterator i = this->upper_bound(KeyOfValue()(mval));
      i = this->m_data.m_vect.insert(i, boost::move(mval));
      return i;
   }

   iterator insert_unique(const_iterator pos, const value_type& val)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(pos, val, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, val);
      }
      return ret.first;
   }

   iterator insert_unique(const_iterator pos, BOOST_RV_REF(value_type) mval)
   {
      insert_commit_data data;
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(pos, mval, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, boost::move(mval));
      }
      return ret.first;
   }

   iterator insert_equal(const_iterator pos, const value_type& val)
   {
      insert_commit_data data;
      priv_insert_equal_prepare(pos, val, data);
      return priv_insert_commit(data, val);
   }

   iterator insert_equal(const_iterator pos, BOOST_RV_REF(value_type) mval)
   {
      insert_commit_data data;
      priv_insert_equal_prepare(pos, mval, data);
      return priv_insert_commit(data, boost::move(mval));
   }

   template <class InIt>
   void insert_unique(InIt first, InIt last)
   {
      for ( ; first != last; ++first)
         this->insert_unique(*first);
   }

   template <class InIt>
   void insert_equal(InIt first, InIt last)
   {
      typedef typename 
         std::iterator_traits<InIt>::iterator_category ItCat;
      priv_insert_equal(first, last, ItCat());
   }

   #ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   template <class... Args>
   std::pair<iterator, bool> emplace_unique(Args&&... args)
   {
      value_type && val = value_type(boost::forward<Args>(args)...);
      insert_commit_data data;
      std::pair<iterator,bool> ret =
         priv_insert_unique_prepare(val, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, boost::move(val));
      }
      return ret;
   }

   template <class... Args>
   iterator emplace_hint_unique(const_iterator hint, Args&&... args)
   {
      value_type && val = value_type(boost::forward<Args>(args)...);
      insert_commit_data data;
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(hint, val, data);
      if(ret.second){
         ret.first = priv_insert_commit(data, boost::move(val));
      }
      return ret.first;
   }

   template <class... Args>
   iterator emplace_equal(Args&&... args)
   {
      value_type &&val = value_type(boost::forward<Args>(args)...);
      iterator i = this->upper_bound(KeyOfValue()(val));
      i = this->m_data.m_vect.insert(i, boost::move(val));
      return i;
   }

   template <class... Args>
   iterator emplace_hint_equal(const_iterator hint, Args&&... args)
   {
      value_type &&val = value_type(boost::forward<Args>(args)...);
      insert_commit_data data;
      priv_insert_equal_prepare(hint, val, data);
      return priv_insert_commit(data, boost::move(val));
   }

   #else //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   #define BOOST_PP_LOCAL_MACRO(n)                                                        \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
   std::pair<iterator, bool>                                                              \
      emplace_unique(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                  \
   {                                                                                      \
      BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), container_detail::value_init<) value_type         \
         BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), >) vval BOOST_PP_LPAREN_IF(n)                  \
            BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) BOOST_PP_RPAREN_IF(n);  \
      value_type &val = vval;                                                             \
      insert_commit_data data;                                                            \
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(val, data);               \
      if(ret.second){                                                                     \
         ret.first = priv_insert_commit(data, boost::move(val));                          \
      }                                                                                   \
      return ret;                                                                         \
   }                                                                                      \
                                                                                          \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
   iterator emplace_hint_unique(const_iterator hint                                       \
                        BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))      \
   {                                                                                      \
      BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), container_detail::value_init<) value_type         \
         BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), >) vval BOOST_PP_LPAREN_IF(n)                  \
            BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) BOOST_PP_RPAREN_IF(n);  \
      value_type &val = vval;                                                             \
      insert_commit_data data;                                                            \
      std::pair<iterator,bool> ret = priv_insert_unique_prepare(hint, val, data);         \
      if(ret.second){                                                                     \
         ret.first = priv_insert_commit(data, boost::move(val));                          \
      }                                                                                   \
      return ret.first;                                                                   \
   }                                                                                      \
                                                                                          \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
   iterator emplace_equal(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))             \
   {                                                                                      \
      BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), container_detail::value_init<) value_type         \
         BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), >) vval BOOST_PP_LPAREN_IF(n)                  \
            BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) BOOST_PP_RPAREN_IF(n);  \
      value_type &val = vval;                                                             \
      iterator i = this->upper_bound(KeyOfValue()(val));                                  \
      i = this->m_data.m_vect.insert(i, boost::move(val));                                \
      return i;                                                                           \
   }                                                                                      \
                                                                                          \
   BOOST_PP_EXPR_IF(n, template<) BOOST_PP_ENUM_PARAMS(n, class P) BOOST_PP_EXPR_IF(n, >) \
   iterator emplace_hint_equal(const_iterator hint                                        \
                      BOOST_PP_ENUM_TRAILING(n, BOOST_CONTAINER_PP_PARAM_LIST, _))        \
   {                                                                                      \
      BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), container_detail::value_init<) value_type         \
         BOOST_PP_EXPR_IF(BOOST_PP_NOT(n), >) vval BOOST_PP_LPAREN_IF(n)                  \
            BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _) BOOST_PP_RPAREN_IF(n);  \
      value_type &val = vval;                                                             \
      insert_commit_data data;                                                            \
      priv_insert_equal_prepare(hint, val, data);                                         \
      return priv_insert_commit(data, boost::move(val));                                  \
   }                                                                                      \
   //!
   #define BOOST_PP_LOCAL_LIMITS (0, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()

   #endif   //#ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   iterator erase(const_iterator position)
   {  return this->m_data.m_vect.erase(position);  }

   size_type erase(const key_type& k)
   {
      std::pair<iterator,iterator > itp = this->equal_range(k);
      size_type ret = static_cast<size_type>(itp.second-itp.first);
      if (ret){
         this->m_data.m_vect.erase(itp.first, itp.second);
      }
      return ret;
   }

   iterator erase(const_iterator first, const_iterator last)
   {  return this->m_data.m_vect.erase(first, last);  }

   void clear()
   {  this->m_data.m_vect.clear();  }

   //! <b>Effects</b>: Tries to deallocate the excess of memory created
   //    with previous allocations. The size of the vector is unchanged
   //!
   //! <b>Throws</b>: If memory allocation throws, or T's copy constructor throws.
   //!
   //! <b>Complexity</b>: Linear to size().
   void shrink_to_fit()
   {  this->m_data.m_vect.shrink_to_fit();  }

   // set operations:
   iterator find(const key_type& k)
   {
      const Compare &key_comp = this->m_data.get_comp();
      iterator i = this->lower_bound(k);

      if (i != this->end() && key_comp(k, KeyOfValue()(*i))){  
         i = this->end();  
      }
      return i;
   }

   const_iterator find(const key_type& k) const
   {
      const Compare &key_comp = this->m_data.get_comp();
      const_iterator i = this->lower_bound(k);

      if (i != this->end() && key_comp(k, KeyOfValue()(*i))){  
         i = this->end();  
      }
      return i;
   }

   size_type count(const key_type& k) const
   {
      std::pair<const_iterator, const_iterator> p = this->equal_range(k);
      size_type n = p.second - p.first;
      return n;
   }

   iterator lower_bound(const key_type& k)
   {  return this->priv_lower_bound(this->begin(), this->end(), k);  }

   const_iterator lower_bound(const key_type& k) const
   {  return this->priv_lower_bound(this->begin(), this->end(), k);  }

   iterator upper_bound(const key_type& k)
   {  return this->priv_upper_bound(this->begin(), this->end(), k);  }

   const_iterator upper_bound(const key_type& k) const
   {  return this->priv_upper_bound(this->begin(), this->end(), k);  }

   std::pair<iterator,iterator> equal_range(const key_type& k)
   {  return this->priv_equal_range(this->begin(), this->end(), k);  }

   std::pair<const_iterator, const_iterator> equal_range(const key_type& k) const
   {  return this->priv_equal_range(this->begin(), this->end(), k);  }

   size_type capacity() const           
   { return this->m_data.m_vect.capacity(); }

   void reserve(size_type count)       
   { this->m_data.m_vect.reserve(count);   }

   private:
   struct insert_commit_data
   {
      const_iterator position;
   };

   // insert/erase
   void priv_insert_equal_prepare
      (const_iterator pos, const value_type& val, insert_commit_data &data)
   {
      // N1780
      //   To insert val at pos:
      //   if pos == end || val <= *pos
      //      if pos == begin || val >= *(pos-1)
      //         insert val before pos
      //      else
      //         insert val before upper_bound(val)
      //   else
      //      insert val before lower_bound(val)
      const value_compare &value_comp = this->m_data;

      if(pos == this->cend() || !value_comp(*pos, val)){
         if (pos == this->cbegin() || !value_comp(val, pos[-1])){
            data.position = pos;
         }
         else{
            data.position = 
               this->priv_upper_bound(this->cbegin(), pos, KeyOfValue()(val));
         }
      }
      else{
         data.position = 
            this->priv_lower_bound(pos, this->cend(), KeyOfValue()(val));
      }
   }

   std::pair<iterator,bool> priv_insert_unique_prepare
      (const_iterator beg, const_iterator end, const value_type& val, insert_commit_data &commit_data)
   {
      const value_compare &value_comp  = this->m_data;
      commit_data.position = this->priv_lower_bound(beg, end, KeyOfValue()(val));
      return std::pair<iterator,bool>
         ( *reinterpret_cast<iterator*>(&commit_data.position)
         , commit_data.position == end || value_comp(val, *commit_data.position));
   }

   std::pair<iterator,bool> priv_insert_unique_prepare
      (const value_type& val, insert_commit_data &commit_data)
   {  return priv_insert_unique_prepare(this->begin(), this->end(), val, commit_data);   }

   std::pair<iterator,bool> priv_insert_unique_prepare
      (const_iterator pos, const value_type& val, insert_commit_data &commit_data)
   {
      //N1780. Props to Howard Hinnant!
      //To insert val at pos:
      //if pos == end || val <= *pos
      //   if pos == begin || val >= *(pos-1)
      //      insert val before pos
      //   else
      //      insert val before upper_bound(val)
      //else if pos+1 == end || val <= *(pos+1)
      //   insert val after pos
      //else
      //   insert val before lower_bound(val)
      const value_compare &value_comp = this->m_data;

      if(pos == this->cend() || value_comp(val, *pos)){
         if(pos != this->cbegin() && !value_comp(val, pos[-1])){
            if(value_comp(pos[-1], val)){
               commit_data.position = pos;
               return std::pair<iterator,bool>(*reinterpret_cast<iterator*>(&pos), true);
            }
            else{
               return std::pair<iterator,bool>(*reinterpret_cast<iterator*>(&pos), false);
            }
         }
         return this->priv_insert_unique_prepare(this->cbegin(), pos, val, commit_data);
      }

      // Works, but increases code complexity
      //Next check
      //else if (value_comp(*pos, val) && !value_comp(pos[1], val)){
      //   if(value_comp(val, pos[1])){
      //      commit_data.position = pos+1;
      //      return std::pair<iterator,bool>(pos+1, true);
      //   }
      //   else{
      //      return std::pair<iterator,bool>(pos+1, false);
      //   }
      //}
      else{
         //[... pos ... val ... ]
         //The hint is before the insertion position, so insert it
         //in the remaining range
         return this->priv_insert_unique_prepare(pos, this->end(), val, commit_data);
      }
   }

   template<class Convertible>
   iterator priv_insert_commit
      (insert_commit_data &commit_data, BOOST_FWD_REF(Convertible) convertible)
   {
      return this->m_data.m_vect.insert
         ( commit_data.position
         , boost::forward<Convertible>(convertible));
   }

   template <class RanIt>
   RanIt priv_lower_bound(RanIt first, RanIt last,
                          const key_type & key) const
   {
      const Compare &key_comp = this->m_data.get_comp();
      KeyOfValue key_extract;
      difference_type len = last - first, half;
      RanIt middle;

      while (len > 0) {
         half = len >> 1;
         middle = first;
         middle += half;

         if (key_comp(key_extract(*middle), key)) {
            ++middle;
            first = middle;
            len = len - half - 1;
         }
         else
            len = half;
      }
      return first;
   }

   template <class RanIt>
   RanIt priv_upper_bound(RanIt first, RanIt last,
                          const key_type & key) const
   {
      const Compare &key_comp = this->m_data.get_comp();
      KeyOfValue key_extract;
      difference_type len = last - first, half;
      RanIt middle;

      while (len > 0) {
         half = len >> 1;
         middle = first;
         middle += half;

         if (key_comp(key, key_extract(*middle))) {
            len = half;
         }
         else{
            first = ++middle;
            len = len - half - 1;  
         }
      }
      return first;
   }

   template <class RanIt>
   std::pair<RanIt, RanIt>
      priv_equal_range(RanIt first, RanIt last, const key_type& key) const
   {
      const Compare &key_comp = this->m_data.get_comp();
      KeyOfValue key_extract;
      difference_type len = last - first, half;
      RanIt middle, left, right;

      while (len > 0) {
         half = len >> 1;
         middle = first;
         middle += half;

         if (key_comp(key_extract(*middle), key)){
            first = middle;
            ++first;
            len = len - half - 1;
         }
         else if (key_comp(key, key_extract(*middle))){
            len = half;
         }
         else {
            left = this->priv_lower_bound(first, middle, key);
            first += len;
            right = this->priv_upper_bound(++middle, first, key);
            return std::pair<RanIt, RanIt>(left, right);
         }
      }
      return std::pair<RanIt, RanIt>(first, first);
   }

   template <class FwdIt>
   void priv_insert_equal(FwdIt first, FwdIt last, std::forward_iterator_tag)
   {
      size_type len = static_cast<size_type>(std::distance(first, last));
      this->reserve(this->size()+len);
      this->priv_insert_equal(first, last, std::input_iterator_tag());
   }

   template <class InIt>
   void priv_insert_equal(InIt first, InIt last, std::input_iterator_tag)
   {
      for ( ; first != last; ++first)
         this->insert_equal(*first);
   }
};

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator==(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
           const flat_tree<Key,Value,KeyOfValue,Compare,A>& y)
{
  return x.size() == y.size() &&
         std::equal(x.begin(), x.end(), y.begin());
}

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator<(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
          const flat_tree<Key,Value,KeyOfValue,Compare,A>& y)
{
  return std::lexicographical_compare(x.begin(), x.end(), 
                                      y.begin(), y.end());
}

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator!=(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
           const flat_tree<Key,Value,KeyOfValue,Compare,A>& y) 
   {  return !(x == y); }

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator>(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
          const flat_tree<Key,Value,KeyOfValue,Compare,A>& y) 
   {  return y < x;  }

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator<=(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
           const flat_tree<Key,Value,KeyOfValue,Compare,A>& y) 
   {  return !(y < x);  }

template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline bool 
operator>=(const flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
           const flat_tree<Key,Value,KeyOfValue,Compare,A>& y) 
   {  return !(x < y);  }


template <class Key, class Value, class KeyOfValue, 
          class Compare, class A>
inline void 
swap(flat_tree<Key,Value,KeyOfValue,Compare,A>& x, 
     flat_tree<Key,Value,KeyOfValue,Compare,A>& y)
   {  x.swap(y);  }

}  //namespace container_detail {

}  //namespace container {
/*
//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class K, class V, class KOV, 
class C, class A>
struct has_trivial_destructor_after_move<boost::container::container_detail::flat_tree<K, V, KOV, C, A> >
{
   static const bool value = has_trivial_destructor<A>::value && has_trivial_destructor<C>::value;
};
*/
}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif // BOOST_CONTAINER_FLAT_TREE_HPP
