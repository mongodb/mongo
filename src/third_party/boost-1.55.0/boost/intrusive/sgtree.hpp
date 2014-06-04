/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
//
// The option that yields to non-floating point 1/sqrt(2) alpha is taken
// from the scapegoat tree implementation of the PSPP library.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_SGTREE_HPP
#define BOOST_INTRUSIVE_SGTREE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <utility>
#include <cmath>
#include <cstddef>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/bs_set_hook.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/utilities.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/sgtree_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/move/move.hpp>

namespace boost {
namespace intrusive {

/// @cond

namespace detail{

//! Returns floor(log2(n)/log2(sqrt(2))) -> floor(2*log2(n))
//! Undefined if N is 0.
//!
//! This function does not use float point operations.
inline std::size_t calculate_h_sqrt2 (std::size_t n)
{
   std::size_t f_log2 = detail::floor_log2(n);
   return (2*f_log2) + (n >= detail::sqrt2_pow_2xplus1 (f_log2));
}

struct h_alpha_sqrt2_t
{
   h_alpha_sqrt2_t(void){}
   std::size_t operator()(std::size_t n) const
   {  return calculate_h_sqrt2(n);  }
};

struct alpha_0_75_by_max_size_t
{
   alpha_0_75_by_max_size_t(void){}

   std::size_t operator()(std::size_t max_tree_size) const
   {
      const std::size_t max_tree_size_limit = ((~std::size_t(0))/std::size_t(3));
      return max_tree_size > max_tree_size_limit ? max_tree_size/4*3 : max_tree_size*3/4;
   }
};

struct h_alpha_t
{
   explicit h_alpha_t(float inv_minus_logalpha)
      :  inv_minus_logalpha_(inv_minus_logalpha)
   {}

   std::size_t operator()(std::size_t n) const
   {
      //Returns floor(log2(1/alpha(n))) ->
      // floor(log2(n)/log(1/alpha)) ->
      // floor(log2(n)/(-log2(alpha)))
      //return static_cast<std::size_t>(std::log(float(n))*inv_minus_logalpha_);
      return static_cast<std::size_t>(detail::fast_log2(float(n))*inv_minus_logalpha_);
   }

   private:
   //Since the function will be repeatedly called
   //precalculate constant data to avoid repeated
   //calls to log and division.
   //This will store 1/(-std::log2(alpha_))
   float inv_minus_logalpha_;
};

struct alpha_by_max_size_t
{
   explicit alpha_by_max_size_t(float alpha)
      :  alpha_(alpha)
   {}

   float operator()(std::size_t max_tree_size) const
   {  return float(max_tree_size)*alpha_;   }

   private:
   float alpha_;
};

template<bool Activate, class SizeType>
struct alpha_holder
{
   typedef boost::intrusive::detail::h_alpha_t           h_alpha_t;
   typedef boost::intrusive::detail::alpha_by_max_size_t multiply_by_alpha_t;

   alpha_holder() : max_tree_size_(0)
   {  set_alpha(0.7f);   }

   float get_alpha() const
   {  return alpha_;  }

   void set_alpha(float alpha)
   {
      alpha_ = alpha;
      inv_minus_logalpha_ = 1/(-detail::fast_log2(alpha));
   }

   h_alpha_t get_h_alpha_t() const
   {  return h_alpha_t(/*alpha_, */inv_minus_logalpha_);  }

   multiply_by_alpha_t get_multiply_by_alpha_t() const
   {  return multiply_by_alpha_t(alpha_);  }

   protected:
   float alpha_;
   float inv_minus_logalpha_;
   SizeType max_tree_size_;
};

template<class SizeType>
struct alpha_holder<false, SizeType>
{
   //This specialization uses alpha = 1/sqrt(2)
   //without using floating point operations
   //Downside: alpha CAN't be changed.
   typedef boost::intrusive::detail::h_alpha_sqrt2_t           h_alpha_t;
   typedef boost::intrusive::detail::alpha_0_75_by_max_size_t  multiply_by_alpha_t;

   float get_alpha() const
   {  return 0.70710677f;  }

   void set_alpha(float)
   {  //alpha CAN't be changed.
      BOOST_INTRUSIVE_INVARIANT_ASSERT(0);
   }

   h_alpha_t get_h_alpha_t() const
   {  return h_alpha_t();  }

   multiply_by_alpha_t get_multiply_by_alpha_t() const
   {  return multiply_by_alpha_t();  }

   SizeType max_tree_size_;
};

}  //namespace detail{

struct sgtree_defaults
{
   typedef detail::default_bstree_hook proto_value_traits;
   static const bool constant_time_size = true;
   typedef std::size_t size_type;
   typedef void compare;
   static const bool floating_point = true;
};

/// @endcond

//! The class template sgtree is an intrusive scapegoat tree container, that
//! is used to construct intrusive sg_set and sg_multiset containers.
//! The no-throw guarantee holds only, if the value_compare object
//! doesn't throw.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c floating_point<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class ValueTraits, class VoidOrKeyComp, class SizeType, bool FloatingPoint>
#endif
class sgtree_impl
   /// @cond
   :  public bstree_impl<ValueTraits, VoidOrKeyComp, SizeType, true, SgTreeAlgorithms>
   ,  public detail::alpha_holder<FloatingPoint, SizeType>
   /// @endcond
{
   public:
   typedef ValueTraits                                               value_traits;
   /// @cond
   typedef bstree_impl< ValueTraits, VoidOrKeyComp, SizeType
                      , true, SgTreeAlgorithms>                      tree_type;
   typedef tree_type                                                 implementation_defined;
   typedef typename tree_type::real_value_traits                     real_value_traits;

   /// @endcond

   typedef typename implementation_defined::pointer                  pointer;
   typedef typename implementation_defined::const_pointer            const_pointer;
   typedef typename implementation_defined::value_type               value_type;
   typedef typename implementation_defined::key_type                 key_type;
   typedef typename implementation_defined::reference                reference;
   typedef typename implementation_defined::const_reference          const_reference;
   typedef typename implementation_defined::difference_type          difference_type;
   typedef typename implementation_defined::size_type                size_type;
   typedef typename implementation_defined::value_compare            value_compare;
   typedef typename implementation_defined::key_compare              key_compare;
   typedef typename implementation_defined::iterator                 iterator;
   typedef typename implementation_defined::const_iterator           const_iterator;
   typedef typename implementation_defined::reverse_iterator         reverse_iterator;
   typedef typename implementation_defined::const_reverse_iterator   const_reverse_iterator;
   typedef typename implementation_defined::node_traits              node_traits;
   typedef typename implementation_defined::node                     node;
   typedef typename implementation_defined::node_ptr                 node_ptr;
   typedef typename implementation_defined::const_node_ptr           const_node_ptr;
   typedef BOOST_INTRUSIVE_IMPDEF(sgtree_algorithms<node_traits>)    node_algorithms;

   static const bool constant_time_size      = implementation_defined::constant_time_size;
   static const bool floating_point          = FloatingPoint;
   static const bool stateful_value_traits   = implementation_defined::stateful_value_traits;

   /// @cond
   private:

   //noncopyable
   typedef detail::alpha_holder<FloatingPoint, SizeType>    alpha_traits;
   typedef typename alpha_traits::h_alpha_t                 h_alpha_t;
   typedef typename alpha_traits::multiply_by_alpha_t       multiply_by_alpha_t;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(sgtree_impl)
   BOOST_STATIC_ASSERT(((int)real_value_traits::link_mode != (int)auto_unlink));

   enum { safemode_or_autounlink  =
            (int)real_value_traits::link_mode == (int)auto_unlink   ||
            (int)real_value_traits::link_mode == (int)safe_link     };

   /// @endcond

   public:

   typedef BOOST_INTRUSIVE_IMPDEF(typename node_algorithms::insert_commit_data) insert_commit_data;

   //! @copydoc ::boost::intrusive::bstree::bstree(const value_compare &,const value_traits &)
   explicit sgtree_impl( const value_compare &cmp = value_compare()
                       , const value_traits &v_traits = value_traits())
      :  tree_type(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bool,Iterator,Iterator,const value_compare &,const value_traits &)
   template<class Iterator>
   sgtree_impl( bool unique, Iterator b, Iterator e
              , const value_compare &cmp     = value_compare()
              , const value_traits &v_traits = value_traits())
      : tree_type(cmp, v_traits)
   {
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   sgtree_impl(BOOST_RV_REF(sgtree_impl) x)
      :  tree_type(::boost::move(static_cast<tree_type&>(x))), alpha_traits(x.get_alpha_traits())
   {  std::swap(this->get_alpha_traits(), x.get_alpha_traits());   }

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   sgtree_impl& operator=(BOOST_RV_REF(sgtree_impl) x)
   {
      this->get_alpha_traits() = x.get_alpha_traits();
      return static_cast<sgtree_impl&>(tree_type::operator=(::boost::move(static_cast<tree_type&>(x))));
   }

   /// @cond
   private:

   const alpha_traits &get_alpha_traits() const
   {  return *this;  }

   alpha_traits &get_alpha_traits()
   {  return *this;  }

   h_alpha_t get_h_alpha_func() const
   {  return this->get_alpha_traits().get_h_alpha_t();  }

   multiply_by_alpha_t get_alpha_by_max_size_func() const
   {  return this->get_alpha_traits().get_multiply_by_alpha_t(); }

   /// @endcond

   public:

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~sgtree_impl();

   //! @copydoc ::boost::intrusive::bstree::begin()
   iterator begin();

   //! @copydoc ::boost::intrusive::bstree::begin()const
   const_iterator begin() const;

   //! @copydoc ::boost::intrusive::bstree::cbegin()const
   const_iterator cbegin() const;

   //! @copydoc ::boost::intrusive::bstree::end()
   iterator end();

   //! @copydoc ::boost::intrusive::bstree::end()const
   const_iterator end() const;

   //! @copydoc ::boost::intrusive::bstree::cend()const
   const_iterator cend() const;

   //! @copydoc ::boost::intrusive::bstree::rbegin()
   reverse_iterator rbegin();

   //! @copydoc ::boost::intrusive::bstree::rbegin()const
   const_reverse_iterator rbegin() const;

   //! @copydoc ::boost::intrusive::bstree::crbegin()const
   const_reverse_iterator crbegin() const;

   //! @copydoc ::boost::intrusive::bstree::rend()
   reverse_iterator rend();

   //! @copydoc ::boost::intrusive::bstree::rend()const
   const_reverse_iterator rend() const;

   //! @copydoc ::boost::intrusive::bstree::crend()const
   const_reverse_iterator crend() const;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(iterator)
   static sgtree_impl &container_from_end_iterator(iterator end_iterator);

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const sgtree_impl &container_from_end_iterator(const_iterator end_iterator);

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static sgtree_impl &container_from_iterator(iterator it);

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const sgtree_impl &container_from_iterator(const_iterator it);

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::swap
   void swap(sgtree_impl& other)
   {
      //This can throw
      using std::swap;
      this->tree_type::swap(static_cast<tree_type&>(other));
      swap(this->get_alpha_traits(), other.get_alpha_traits());
   }

   //! @copydoc ::boost::intrusive::bstree::clone_from
   //! Additional notes: it also copies the alpha factor from the source container.
   template <class Cloner, class Disposer>
   void clone_from(const sgtree_impl &src, Cloner cloner, Disposer disposer)
   {
      this->tree_type::clone_from(src, cloner, disposer);
      this->get_alpha_traits() = src.get_alpha_traits();
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(reference)
   iterator insert_equal(reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->value_comp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_ptr p = node_algorithms::insert_equal_upper_bound
         (this->tree_type::header_ptr(), to_insert, key_node_comp
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this->real_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(const_iterator,reference)
   iterator insert_equal(const_iterator hint, reference value)
   {
      detail::key_nodeptr_comp<value_compare, real_value_traits>
         key_node_comp(this->value_comp(), &this->get_real_value_traits());
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_ptr p = node_algorithms::insert_equal
         (this->tree_type::header_ptr(), hint.pointed_node(), to_insert, key_node_comp
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this->real_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      iterator iend(this->end());
      for (; b != e; ++b)
         this->insert_equal(iend, *b);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(reference)
   std::pair<iterator, bool> insert_unique(reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(value, this->value_comp(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<iterator, bool> (insert_unique_commit(value, commit_data), true);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(const_iterator,reference)
   iterator insert_unique(const_iterator hint, reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(hint, value, this->value_comp(), commit_data);
      if(!ret.second)
         return ret.first;
      return insert_unique_commit(value, commit_data);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const KeyType&,KeyValueCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         comp(key_value_comp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->tree_type::header_ptr(), key, comp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const KeyType&,KeyValueCompare,insert_commit_data&)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, real_value_traits>
         comp(key_value_comp, &this->get_real_value_traits());
      std::pair<node_ptr, bool> ret =
         (node_algorithms::insert_unique_check
            (this->tree_type::header_ptr(), hint.pointed_node(), key, comp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this->real_value_traits_ptr()), ret.second);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_commit
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_algorithms::insert_unique_commit
         ( this->tree_type::header_ptr(), to_insert, commit_data
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
      return iterator(to_insert, this->real_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      if(this->empty()){
         iterator iend(this->end());
         for (; b != e; ++b)
            this->insert_unique(iend, *b);
      }
      else{
         for (; b != e; ++b)
            this->insert_unique(*b);
      }
   }

   //! @copydoc ::boost::intrusive::bstree::insert_before
   iterator insert_before(const_iterator pos, reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_ptr p = node_algorithms::insert_before
         ( this->tree_type::header_ptr(), pos.pointed_node(), to_insert
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this->real_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::push_back
   void push_back(reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_algorithms::push_back
         ( this->tree_type::header_ptr(), to_insert
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
   }

   //! @copydoc ::boost::intrusive::bstree::push_front
   void push_front(reference value)
   {
      node_ptr to_insert(this->get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      node_algorithms::push_front
         ( this->tree_type::header_ptr(), to_insert
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->tree_type::sz_traits().increment();
      this->max_tree_size_ = (size_type)max_tree_size;
   }


   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator)
   iterator erase(const_iterator i)
   {
      const_iterator ret(i);
      ++ret;
      node_ptr to_erase(i.pointed_node());
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!node_algorithms::unique(to_erase));
      std::size_t max_tree_size = this->max_tree_size_;
      node_algorithms::erase
         ( this->tree_type::header_ptr(), to_erase, (std::size_t)this->size()
         , max_tree_size, this->get_alpha_by_max_size_func());
      this->max_tree_size_ = (size_type)max_tree_size;
      this->tree_type::sz_traits().decrement();
      if(safemode_or_autounlink)
         node_algorithms::init(to_erase);
      return ret.unconst();
   }

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e)
   {  size_type n;   return private_erase(b, e, n);   }

   //! @copydoc ::boost::intrusive::bstree::erase(const_reference)
   size_type erase(const_reference value)
   {  return this->erase(value, this->value_comp());   }

   //! @copydoc ::boost::intrusive::bstree::erase(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   size_type erase(const KeyType& key, KeyValueCompare comp
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {
      std::pair<iterator,iterator> p = this->equal_range(key, comp);
      size_type n;
      private_erase(p.first, p.second, n);
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer)
   {
      node_ptr to_erase(i.pointed_node());
      iterator ret(this->erase(i));
      disposer(this->get_real_value_traits().to_value_ptr(to_erase));
      return ret;
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   iterator erase_and_dispose(iterator i, Disposer disposer)
   {  return this->erase_and_dispose(const_iterator(i), disposer);   }
   #endif

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  size_type n;   return private_erase(b, e, n, disposer);   }

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_reference, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {
      std::pair<iterator,iterator> p = this->equal_range(value);
      size_type n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const KeyType&,KeyValueCompare,Disposer)
   template<class KeyType, class KeyValueCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyValueCompare comp, Disposer disposer
                  /// @cond
                  , typename detail::enable_if_c<!detail::is_convertible<KeyValueCompare, const_iterator>::value >::type * = 0
                  /// @endcond
                  )
   {
      std::pair<iterator,iterator> p = this->equal_range(key, comp);
      size_type n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree::clear
   void clear()
   {
      tree_type::clear();
      this->max_tree_size_ = 0;
   }

   //! @copydoc ::boost::intrusive::bstree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer)
   {
      tree_type::clear_and_dispose(disposer);
      this->max_tree_size_ = 0;
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::count(const_reference)const
   size_type count(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::count(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType& key, KeyValueCompare comp) const;
   
   //! @copydoc ::boost::intrusive::bstree::lower_bound(const_reference)
   iterator lower_bound(const_reference value);
   
   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const_reference)const
   const_iterator lower_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const_reference)
   iterator upper_bound(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const_reference)const
   const_iterator upper_bound(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::find(const_reference)
   iterator find(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::find(const_reference)const
   const_iterator find(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const_reference)
   std::pair<iterator,iterator> equal_range(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyValueCompare)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyValueCompare comp);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const_reference)const
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyValueCompare)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyValueCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const_reference,const_reference,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const_reference,const_reference,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const_reference lower_value, const_reference upper_value, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyValueCompare,bool,bool)const
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyValueCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value);

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value);

   //! @copydoc ::boost::intrusive::bstree::iterator_to(reference)
   iterator iterator_to(reference value);

   //! @copydoc ::boost::intrusive::bstree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const;

   //! @copydoc ::boost::intrusive::bstree::init_node(reference)
   static void init_node(reference value);

   //! @copydoc ::boost::intrusive::bstree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance();

   //! @copydoc ::boost::intrusive::bstree::replace_node
   void replace_node(iterator replace_this, reference with_this);

   //! @copydoc ::boost::intrusive::bstree::remove_node
   void remove_node(reference value);

   //! @copydoc ::boost::intrusive::bstree::rebalance
   void rebalance();

   //! @copydoc ::boost::intrusive::bstree::rebalance_subtree
   iterator rebalance_subtree(iterator root);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Returns</b>: The balance factor (alpha) used in this tree
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   float balance_factor() const
   {  return this->get_alpha_traits().get_alpha(); }

   //! <b>Requires</b>: new_alpha must be a value between 0.5 and 1.0
   //!
   //! <b>Effects</b>: Establishes a new balance factor (alpha) and rebalances
   //!   the tree if the new balance factor is stricter (less) than the old factor.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   void balance_factor(float new_alpha)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT((new_alpha > 0.5f && new_alpha < 1.0f));
      if(new_alpha < 0.5f && new_alpha >= 1.0f)  return;

      //The alpha factor CAN't be changed if the fixed, floating operation-less
      //1/sqrt(2) alpha factor option is activated
      BOOST_STATIC_ASSERT((floating_point));
      float old_alpha = this->get_alpha_traits().get_alpha();
      this->get_alpha_traits().set_alpha(new_alpha);

      if(new_alpha < old_alpha){
         this->max_tree_size_ = this->size();
         this->rebalance();
      }
   }

   /// @cond
   private:
   template<class Disposer>
   iterator private_erase(const_iterator b, const_iterator e, size_type &n, Disposer disposer)
   {
      for(n = 0; b != e; ++n)
        this->erase_and_dispose(b++, disposer);
      return b.unconst();
   }

   iterator private_erase(const_iterator b, const_iterator e, size_type &n)
   {
      for(n = 0; b != e; ++n)
        this->erase(b++);
      return b.unconst();
   }
   /// @endcond
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator< (const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator==(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator!= (const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(sgtree_impl<T, Options...> &x, sgtree_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c sgtree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void>
#endif
struct make_sgtree
{
   /// @cond
   typedef typename pack_options
      < sgtree_defaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type packed_options;

   typedef typename detail::get_value_traits
      <T, typename packed_options::proto_value_traits>::type value_traits;

   typedef sgtree_impl
         < value_traits
         , typename packed_options::compare
         , typename packed_options::size_type
         , packed_options::floating_point
         > implementation_defined;
   /// @endcond
   typedef implementation_defined type;
};


#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4>
#else
template<class T, class ...Options>
#endif
class sgtree
   :  public make_sgtree<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type
{
   typedef typename make_sgtree
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(sgtree)

   public:
   typedef typename Base::value_compare      value_compare;
   typedef typename Base::value_traits       value_traits;
   typedef typename Base::real_value_traits  real_value_traits;
   typedef typename Base::iterator           iterator;
   typedef typename Base::const_iterator     const_iterator;
   typedef typename Base::reverse_iterator           reverse_iterator;
   typedef typename Base::const_reverse_iterator     const_reverse_iterator;

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename real_value_traits::value_type, T>::value));

   explicit sgtree( const value_compare &cmp = value_compare()
                  , const value_traits &v_traits = value_traits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   sgtree( bool unique, Iterator b, Iterator e
         , const value_compare &cmp = value_compare()
         , const value_traits &v_traits = value_traits())
      :  Base(unique, b, e, cmp, v_traits)
   {}

   sgtree(BOOST_RV_REF(sgtree) x)
      :  Base(::boost::move(static_cast<Base&>(x)))
   {}

   sgtree& operator=(BOOST_RV_REF(sgtree) x)
   {  return static_cast<sgtree &>(this->Base::operator=(::boost::move(static_cast<Base&>(x))));  }

   static sgtree &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<sgtree &>(Base::container_from_end_iterator(end_iterator));   }

   static const sgtree &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const sgtree &>(Base::container_from_end_iterator(end_iterator));   }

   static sgtree &container_from_iterator(iterator it)
   {  return static_cast<sgtree &>(Base::container_from_iterator(it));   }

   static const sgtree &container_from_iterator(const_iterator it)
   {  return static_cast<const sgtree &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SGTREE_HPP
