/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2009
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
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/clear_on_destructor_base.hpp>
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

//! Returns floor(log(n)/log(sqrt(2))) -> floor(2*log2(n))
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
   h_alpha_t(float inv_minus_logalpha)
      :  inv_minus_logalpha_(inv_minus_logalpha)
   {}

   std::size_t operator()(std::size_t n) const
   {
      //Returns floor(log1/alpha(n)) ->
      // floor(log(n)/log(1/alpha)) ->
      // floor(log(n)/(-log(alpha)))
      //return static_cast<std::size_t>(std::log(float(n))*inv_minus_logalpha_);
      return static_cast<std::size_t>(detail::fast_log2(float(n))*inv_minus_logalpha_);
   }

   private:
   //Since the function will be repeatedly called
   //precalculate constant data to avoid repeated
   //calls to log and division.
   //This will store 1/(-std::log(alpha_))
   float inv_minus_logalpha_;
};

struct alpha_by_max_size_t
{
   alpha_by_max_size_t(float alpha)
      :  alpha_(alpha)
   {}
   
   float operator()(std::size_t max_tree_size) const
   {  return float(max_tree_size)*alpha_;   }

   private:
   float alpha_;
   float inv_minus_logalpha_;
};

template<bool Activate>
struct alpha_holder
{
   typedef boost::intrusive::detail::h_alpha_t           h_alpha_t;
   typedef boost::intrusive::detail::alpha_by_max_size_t multiply_by_alpha_t;

   alpha_holder()
   {  set_alpha(0.7f);   }

   float get_alpha() const
   {  return alpha_;  }

   void set_alpha(float alpha)
   { 
      alpha_ = alpha;
      inv_minus_logalpha_ = 1/(-detail::fast_log2(alpha));
   }

   h_alpha_t get_h_alpha_t() const
   {  return h_alpha_t(inv_minus_logalpha_);  }

   multiply_by_alpha_t get_multiply_by_alpha_t() const
   {  return multiply_by_alpha_t(alpha_);  }

   private:
   float alpha_;
   float inv_minus_logalpha_;
};

template<>
struct alpha_holder<false>
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
};

}  //namespace detail{

template <class ValueTraits, class Compare, class SizeType, bool FloatingPoint>
struct sg_setopt
{
   typedef ValueTraits  value_traits;
   typedef Compare      compare;
   typedef SizeType     size_type;
   static const bool floating_point = FloatingPoint;
};

template <class T>
struct sg_set_defaults
   :  pack_options
      < none
      , base_hook<detail::default_bs_set_hook>
      , floating_point<true>
      , size_type<std::size_t>
      , compare<std::less<T> >
      >::type
{};

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
template<class Config>
#endif
class sgtree_impl
   :  private detail::clear_on_destructor_base<sgtree_impl<Config> >
{
   template<class C> friend class detail::clear_on_destructor_base;
   public:
   typedef typename Config::value_traits                             value_traits;
   /// @cond
   static const bool external_value_traits =
      detail::external_value_traits_is_true<value_traits>::value;
   typedef typename detail::eval_if_c
      < external_value_traits
      , detail::eval_value_traits<value_traits>
      , detail::identity<value_traits>
      >::type                                                        real_value_traits;
   /// @endcond
   typedef typename real_value_traits::pointer                       pointer;
   typedef typename real_value_traits::const_pointer                 const_pointer;
   typedef typename pointer_traits<pointer>::element_type            value_type;
   typedef value_type                                                key_type;
   typedef typename pointer_traits<pointer>::reference               reference;
   typedef typename pointer_traits<const_pointer>::reference         const_reference;
   typedef typename pointer_traits<const_pointer>::difference_type   difference_type;
   typedef typename Config::size_type                                size_type;
   typedef typename Config::compare                                  value_compare;
   typedef value_compare                                             key_compare;
   typedef tree_iterator<sgtree_impl, false>                         iterator;
   typedef tree_iterator<sgtree_impl, true>                          const_iterator;
   typedef boost::intrusive::detail::reverse_iterator<iterator>      reverse_iterator;
   typedef boost::intrusive::detail::reverse_iterator<const_iterator>const_reverse_iterator;
   typedef typename real_value_traits::node_traits                   node_traits;
   typedef typename node_traits::node                                node;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         <node>::type                                                node_ptr;
   typedef typename pointer_traits
      <pointer>::template rebind_pointer
         <const node>::type                                          const_node_ptr;
   typedef sgtree_algorithms<node_traits>                            node_algorithms;

   static const bool floating_point    = Config::floating_point;
   static const bool constant_time_size    = true;
   static const bool stateful_value_traits = detail::is_stateful_value_traits<real_value_traits>::value;

   /// @cond
   private:
   typedef detail::size_holder<true, size_type>          size_traits;
   typedef detail::alpha_holder<floating_point>      alpha_traits;
   typedef typename alpha_traits::h_alpha_t              h_alpha_t;
   typedef typename alpha_traits::multiply_by_alpha_t    multiply_by_alpha_t;

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(sgtree_impl)

   enum { safemode_or_autounlink  = 
            (int)real_value_traits::link_mode == (int)auto_unlink   ||
            (int)real_value_traits::link_mode == (int)safe_link     };

   BOOST_STATIC_ASSERT(((int)real_value_traits::link_mode != (int)auto_unlink));

   //BOOST_STATIC_ASSERT((
   //                     (int)real_value_traits::link_mode != (int)auto_unlink ||
   //                     !floating_point
   //                   ));

   struct header_plus_alpha : public alpha_traits
   {  node header_;  };

   struct node_plus_pred_t : public detail::ebo_functor_holder<value_compare>
   {
      node_plus_pred_t(const value_compare &comp)
         :  detail::ebo_functor_holder<value_compare>(comp)
      {}
      header_plus_alpha header_plus_alpha_;
      size_traits size_traits_;
   };

   struct data_t : public sgtree_impl::value_traits
   {
      typedef typename sgtree_impl::value_traits value_traits;
      data_t(const value_compare & comp, const value_traits &val_traits)
         :  value_traits(val_traits), node_plus_pred_(comp)
         ,  max_tree_size_(0)
      {}
      node_plus_pred_t node_plus_pred_;
      size_type max_tree_size_;
   } data_;

   float priv_alpha() const
   {  return this->priv_alpha_traits().get_alpha();  }

   void priv_alpha(float alpha)
   {  return this->priv_alpha_traits().set_alpha(alpha);  }
  
   const value_compare &priv_comp() const
   {  return data_.node_plus_pred_.get();  }

   value_compare &priv_comp()
   {  return data_.node_plus_pred_.get();  }

   const value_traits &priv_value_traits() const
   {  return data_;  }

   value_traits &priv_value_traits()
   {  return data_;  }

   node_ptr priv_header_ptr()
   {  return pointer_traits<node_ptr>::pointer_to(data_.node_plus_pred_.header_plus_alpha_.header_);  }

   const_node_ptr priv_header_ptr() const
   {  return pointer_traits<const_node_ptr>::pointer_to(data_.node_plus_pred_.header_plus_alpha_.header_);  }

   static node_ptr uncast(const const_node_ptr & ptr)
   {  return pointer_traits<node_ptr>::const_cast_from(ptr);  }

   size_traits &priv_size_traits()
   {  return data_.node_plus_pred_.size_traits_;  }

   const size_traits &priv_size_traits() const
   {  return data_.node_plus_pred_.size_traits_;  }

   alpha_traits &priv_alpha_traits()
   {  return data_.node_plus_pred_.header_plus_alpha_;  }

   const alpha_traits &priv_alpha_traits() const
   {  return data_.node_plus_pred_.header_plus_alpha_;  }

   const real_value_traits &get_real_value_traits(detail::bool_<false>) const
   {  return data_;  }

   const real_value_traits &get_real_value_traits(detail::bool_<true>) const
   {  return data_.get_value_traits(*this);  }

   real_value_traits &get_real_value_traits(detail::bool_<false>)
   {  return data_;  }

   real_value_traits &get_real_value_traits(detail::bool_<true>)
   {  return data_.get_value_traits(*this);  }

   h_alpha_t get_h_alpha_func() const
   {  return priv_alpha_traits().get_h_alpha_t();  }

   multiply_by_alpha_t get_alpha_by_max_size_func() const
   {  return priv_alpha_traits().get_multiply_by_alpha_t(); }

   /// @endcond

   public:

   const real_value_traits &get_real_value_traits() const
   {  return this->get_real_value_traits(detail::bool_<external_value_traits>());  }

   real_value_traits &get_real_value_traits()
   {  return this->get_real_value_traits(detail::bool_<external_value_traits>());  }

   typedef typename node_algorithms::insert_commit_data insert_commit_data;

   //! <b>Effects</b>: Constructs an empty tree. 
   //!   
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructorof the value_compare object throws. Basic guarantee.
   sgtree_impl( const value_compare &cmp     = value_compare()
              , const value_traits &v_traits = value_traits()) 
      :  data_(cmp, v_traits)
   {  
      node_algorithms::init_header(this->priv_header_ptr());  
      this->priv_size_traits().set_size(size_type(0));
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty tree and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is the distance between first and last.
   //! 
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the value_compare object throws. Basic guarantee.
   template<class Iterator>
   sgtree_impl( bool unique, Iterator b, Iterator e
              , const value_compare &cmp     = value_compare()
              , const value_traits &v_traits = value_traits())
      : data_(cmp, v_traits)
   {
      node_algorithms::init_header(this->priv_header_ptr());
      this->priv_size_traits().set_size(size_type(0));
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! <b>Effects</b>: to-do
   //!   
   sgtree_impl(BOOST_RV_REF(sgtree_impl) x)
      : data_(::boost::move(x.priv_comp()), ::boost::move(x.priv_value_traits()))
   {
      node_algorithms::init_header(this->priv_header_ptr());  
      this->priv_size_traits().set_size(size_type(0));
      this->swap(x);
   }

   //! <b>Effects</b>: to-do
   //!   
   sgtree_impl& operator=(BOOST_RV_REF(sgtree_impl) x) 
   {  this->swap(x); return *this;  }

   //! <b>Effects</b>: Detaches all elements from this. The objects in the set 
   //!   are not deleted (i.e. no destructors are called), but the nodes according to 
   //!   the value_traits template parameter are reinitialized and thus can be reused. 
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this. 
   //! 
   //! <b>Throws</b>: Nothing.
   ~sgtree_impl() 
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator begin()
   {  return iterator (node_traits::get_left(this->priv_header_ptr()), this);   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const
   {  return cbegin();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const
   {  return const_iterator (node_traits::get_left(this->priv_header_ptr()), this);   }

   //! <b>Effects</b>: Returns an iterator pointing to the end of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator end()
   {  return iterator (this->priv_header_ptr(), this);  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the tree.
   //!
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator end() const
   {  return cend();  }

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const
   {  return const_iterator (uncast(this->priv_header_ptr()), this);  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin()
   {  return reverse_iterator(end());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const
   {  return const_reverse_iterator(end());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const
   {  return const_reverse_iterator(end());  }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend()
   {  return reverse_iterator(begin());   }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const
   {  return const_reverse_iterator(begin());   }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const
   {  return const_reverse_iterator(begin());   }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of sgtree.
   //! 
   //! <b>Effects</b>: Returns a const reference to the sgtree associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static sgtree_impl &container_from_end_iterator(iterator end_iterator)
   {  return priv_container_from_end_iterator(end_iterator);   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of sgtree.
   //! 
   //! <b>Effects</b>: Returns a const reference to the sgtree associated to the end iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   static const sgtree_impl &container_from_end_iterator(const_iterator end_iterator)
   {  return priv_container_from_end_iterator(end_iterator);   }

   //! <b>Precondition</b>: it must be a valid iterator
   //!   of rbtree.
   //! 
   //! <b>Effects</b>: Returns a const reference to the tree associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static sgtree_impl &container_from_iterator(iterator it)
   {  return priv_container_from_iterator(it);   }

   //! <b>Precondition</b>: it must be a valid end const_iterator
   //!   of rbtree.
   //! 
   //! <b>Effects</b>: Returns a const reference to the tree associated to the iterator
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   static const sgtree_impl &container_from_iterator(const_iterator it)
   {  return priv_container_from_iterator(it);   }

   //! <b>Effects</b>: Returns the value_compare object used by the tree.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const
   {  return priv_comp();   }

   //! <b>Effects</b>: Returns true if the container is empty.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   bool empty() const
   {  return node_algorithms::unique(this->priv_header_ptr());   }

   //! <b>Effects</b>: Returns the number of elements stored in the tree.
   //! 
   //! <b>Complexity</b>: Linear to elements contained in *this
   //!   if constant-time size option is disabled. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type size() const
   {
      if(constant_time_size)
         return this->priv_size_traits().get_size();
      else{
         return (size_type)node_algorithms::size(this->priv_header_ptr());
      }
   }

   //! <b>Effects</b>: Swaps the contents of two sgtrees.
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: If the comparison functor's swap call throws.
   void swap(sgtree_impl& other)
   {
      //This can throw
      using std::swap;
      swap(priv_comp(), priv_comp());
      swap(priv_alpha_traits(), priv_alpha_traits());
      swap(data_.max_tree_size_, other.data_.max_tree_size_);
      //These can't throw
      node_algorithms::swap_tree(this->priv_header_ptr(), other.priv_header_ptr());
      if(constant_time_size){
         size_type backup = this->priv_size_traits().get_size();
         this->priv_size_traits().set_size(other.priv_size_traits().get_size());
         other.priv_size_traits().set_size(backup);
      }
   }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts value into the tree before the upper bound.
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: If the internal value_compare ordering function throws. Strong guarantee.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(reference value)
   {
      detail::key_nodeptr_comp<value_compare, sgtree_impl>
         key_node_comp(priv_comp(), this);
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_ptr p = node_algorithms::insert_equal_upper_bound
         (this->priv_header_ptr(), to_insert, key_node_comp
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this);
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator.
   //! 
   //! <b>Effects</b>: Inserts x into the tree, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case)
   //! 
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_equal(const_iterator hint, reference value)
   {
      detail::key_nodeptr_comp<value_compare, sgtree_impl>
         key_node_comp(priv_comp(), this);
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_ptr p = node_algorithms::insert_equal
         (this->priv_header_ptr(), hint.pointed_node(), to_insert, key_node_comp
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Inserts a each element of a range into the tree
   //!   before the upper bound of the key of each element.
   //! 
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      iterator end(this->end());
      for (; b != e; ++b)
         this->insert_equal(end, *b);
   }

   //! <b>Requires</b>: value must be an lvalue
   //! 
   //! <b>Effects</b>: Inserts value into the tree if the value
   //!   is not already present.
   //! 
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<iterator, bool> insert_unique(reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(value, priv_comp(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<iterator, bool> (insert_unique_commit(value, commit_data), true);
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator
   //! 
   //! <b>Effects</b>: Tries to insert x into the tree, using "hint" as a hint
   //!   to where it will be inserted.
   //! 
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time (two comparisons in the worst case)
   //!   if t is inserted immediately before hint.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   iterator insert_unique(const_iterator hint, reference value)
   {
      insert_commit_data commit_data;
      std::pair<iterator, bool> ret = insert_unique_check(hint, value, priv_comp(), commit_data);
      if(!ret.second)
         return ret.first;
      return insert_unique_commit(value, commit_data);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue 
   //!   of type value_type.
   //! 
   //! <b>Effects</b>: Tries to insert each element of a range into the tree.
   //! 
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the 
   //!   size of the range. However, it is linear in N if the range is already sorted 
   //!   by value_comp().
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      if(this->empty()){
         iterator end(this->end());
         for (; b != e; ++b)
            this->insert_unique(end, *b);
      }
      else{
         for (; b != e; ++b)
            this->insert_unique(*b);
      }
   }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //! 
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the key_value_comp ordering function throws. Strong guarantee.
   //! 
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that 
   //!   part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         comp(key_value_comp, this);
      std::pair<node_ptr, bool> ret = 
         (node_algorithms::insert_unique_check
            (this->priv_header_ptr(), key, comp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this), ret.second);
   }

   //! <b>Requires</b>: key_value_comp must be a comparison function that induces 
   //!   the same strict weak ordering as value_compare. The difference is that
   //!   key_value_comp compares an arbitrary key with the contained values.
   //! 
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint" 
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //! 
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the key_value_comp ordering function throws. Strong guarantee.
   //! 
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   constructing that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that key 
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This can give a total
   //!   constant-time complexity to the insertion: check(O(1)) + commit(O(1)).
   //!   
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyValueCompare key_value_comp, insert_commit_data &commit_data)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         comp(key_value_comp, this);
      std::pair<node_ptr, bool> ret = 
         (node_algorithms::insert_unique_check
            (this->priv_header_ptr(), hint.pointed_node(), key, comp, commit_data));
      return std::pair<iterator, bool>(iterator(ret.first, this), ret.second);
   }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the container between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //! 
   //! <b>Effects</b>: Inserts the value in the avl_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //! 
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data)
   {
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_algorithms::insert_unique_commit
         ( this->priv_header_ptr(), to_insert, commit_data
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
      return iterator(to_insert, this);
   }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate
   //!
   //! <b>Effects</b>: Inserts x into the tree before "pos".
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" tree ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   iterator insert_before(const_iterator pos, reference value)
   {
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_ptr p = node_algorithms::insert_before
         ( this->priv_header_ptr(), pos.pointed_node(), to_insert
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
      return iterator(p, this);
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key
   //!
   //! <b>Effects</b>: Inserts x into the tree in the last position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key tree ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(reference value)
   {
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_algorithms::push_back
         ( this->priv_header_ptr(), to_insert 
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the tree in the first position.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key tree ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(reference value)
   {
      node_ptr to_insert(get_real_value_traits().to_node_ptr(value));
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(node_algorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)data_.max_tree_size_;
      node_algorithms::push_front
         ( this->priv_header_ptr(), to_insert
         , (size_type)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->priv_size_traits().increment();
      data_.max_tree_size_ = (size_type)max_tree_size;
   }

   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //! 
   //! <b>Complexity</b>: Average complexity for erase element is constant time. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator i)
   {
      const_iterator ret(i);
      ++ret;
      node_ptr to_erase(i.pointed_node());
      if(safemode_or_autounlink)
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!node_algorithms::unique(to_erase));
      std::size_t max_tree_size = data_.max_tree_size_;
      node_algorithms::erase
         ( this->priv_header_ptr(), to_erase, (std::size_t)this->size()
         , max_tree_size, this->get_alpha_by_max_size_func());
      data_.max_tree_size_ = (size_type)max_tree_size;
      this->priv_size_traits().decrement();
      if(safemode_or_autounlink)
         node_algorithms::init(to_erase);
      return ret.unconst();
   }

   //! <b>Effects</b>: Erases the range pointed to by b end e. 
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is at most 
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   iterator erase(const_iterator b, const_iterator e)
   {  size_type n;   return private_erase(b, e, n);   }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + N).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   size_type erase(const_reference value)
   {  return this->erase(value, priv_comp());   }

   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + N).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
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

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by pos. 
   //!   Disposer::operator()(pointer) is called for the removed element.
   //! 
   //! <b>Complexity</b>: Average complexity for erase element is constant time. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators 
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer)
   {
      node_ptr to_erase(i.pointed_node());
      iterator ret(this->erase(i));
      disposer(get_real_value_traits().to_value_ptr(to_erase));
      return ret;
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   iterator erase_and_dispose(iterator i, Disposer disposer)
   {  return this->erase_and_dispose(const_iterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Complexity</b>: Average complexity for erase range is at most 
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer)
   {  size_type n;   return private_erase(b, e, n, disposer);   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + N).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   size_type erase_and_dispose(const_reference value, Disposer disposer)
   {
      std::pair<iterator,iterator> p = this->equal_range(value);
      size_type n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //! 
   //! <b>Complexity</b>: O(log(size() + N).
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
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

   //! <b>Effects</b>: Erases all of the elements. 
   //! 
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear()
   {
      if(safemode_or_autounlink){
         this->clear_and_dispose(detail::null_disposer());
      }
      else{
         node_algorithms::init_header(this->priv_header_ptr());
         this->priv_size_traits().set_size(0);
      }
   }

   //! <b>Effects</b>: Erases all of the elements calling disposer(p) for
   //!   each node to be erased.
   //! <b>Complexity</b>: Average complexity for is at most O(log(size() + N)),
   //!   where N is the number of elements in the container.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. Calls N times to disposer functor.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer)
   {
      node_algorithms::clear_and_dispose(this->priv_header_ptr()
         , detail::node_disposer<Disposer, sgtree_impl>(disposer, this));
      this->priv_size_traits().set_size(0);
   }

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given value.
   //! 
   //! <b>Throws</b>: Nothing.
   size_type count(const_reference value) const
   {  return this->count(value, priv_comp());   }

   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //! 
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   size_type count(const KeyType &key, KeyValueCompare comp) const
   {
      std::pair<const_iterator, const_iterator> ret = this->equal_range(key, comp);
      return std::distance(ret.first, ret.second);
   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator lower_bound(const_reference value)
   {  return this->lower_bound(value, priv_comp());   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator lower_bound(const_reference value) const
   {  return this->lower_bound(value, priv_comp());   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator lower_bound(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return iterator(node_algorithms::lower_bound
         (this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Returns a const iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator lower_bound(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return const_iterator(node_algorithms::lower_bound
         (this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator upper_bound(const_reference value)
   {  return this->upper_bound(value, priv_comp());   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k according to comp or end() if that element
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator upper_bound(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return iterator(node_algorithms::upper_bound
         (this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator upper_bound(const_reference value) const
   {  return this->upper_bound(value, priv_comp());   }

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k according to comp or end() if that element
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator upper_bound(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return const_iterator(node_algorithms::upper_bound
         (this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator find(const_reference value)
   {  return this->find(value, priv_comp()); }

   //! <b>Effects</b>: Finds an iterator to the first element whose key is 
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   iterator find(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return iterator
         (node_algorithms::find(this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator find(const_reference value) const
   {  return this->find(value, priv_comp()); }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is 
   //!   k or end() if that element does not exist.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   const_iterator find(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      return const_iterator
         (node_algorithms::find(this->priv_header_ptr(), key, key_node_comp), this);
   }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   std::pair<iterator,iterator> equal_range(const_reference value)
   {  return this->equal_range(value, priv_comp());   }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   std::pair<iterator,iterator> equal_range(const KeyType &key, KeyValueCompare comp)
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::equal_range(this->priv_header_ptr(), key, key_node_comp));
      return std::pair<iterator, iterator>(iterator(ret.first, this), iterator(ret.second, this));
   }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   std::pair<const_iterator, const_iterator>
      equal_range(const_reference value) const
   {  return this->equal_range(value, priv_comp());   }

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //! 
   //! <b>Complexity</b>: Logarithmic.
   //! 
   //! <b>Throws</b>: Nothing.
   template<class KeyType, class KeyValueCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType &key, KeyValueCompare comp) const
   {
      detail::key_nodeptr_comp<KeyValueCompare, sgtree_impl>
         key_node_comp(comp, this);
      std::pair<node_ptr, node_ptr> ret
         (node_algorithms::equal_range(this->priv_header_ptr(), key, key_node_comp));
      return std::pair<const_iterator, const_iterator>(const_iterator(ret.first, this), const_iterator(ret.second, this));
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the 
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!   
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //! 
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const sgtree_impl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!src.empty()){
         detail::exception_disposer<sgtree_impl, Disposer>
            rollback(*this, disposer);
         node_algorithms::clone
            (src.priv_header_ptr()
            ,this->priv_header_ptr()
            ,detail::node_cloner<Cloner, sgtree_impl>(cloner, this)
            ,detail::node_disposer<Disposer, sgtree_impl>(disposer, this));
         this->priv_size_traits().set_size(src.priv_size_traits().get_size());
         this->priv_comp() = src.priv_comp();
         rollback.release();
      }
   }

   //! <b>Effects</b>: Unlinks the leftmost node from the tree.
   //! 
   //! <b>Complexity</b>: Average complexity is constant time.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Notes</b>: This function breaks the tree and the tree can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the tree.
   pointer unlink_leftmost_without_rebalance()
   {
      node_ptr to_be_disposed(node_algorithms::unlink_leftmost_without_rebalance
                           (this->priv_header_ptr()));
      if(!to_be_disposed)
         return 0;
      this->priv_size_traits().decrement();
      if(safemode_or_autounlink)//If this is commented does not work with normal_link
         node_algorithms::init(to_be_disposed);
      return get_real_value_traits().to_value_ptr(to_be_disposed);
   }

   //! <b>Requires</b>: replace_this must be a valid iterator of *this
   //!   and with_this must not be inserted in any tree.
   //! 
   //! <b>Effects</b>: Replaces replace_this in its position in the
   //!   tree with with_this. The tree does not need to be rebalanced.
   //! 
   //! <b>Complexity</b>: Constant. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   with_this is not equivalent to *replace_this according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   void replace_node(iterator replace_this, reference with_this)
   {
      node_algorithms::replace_node( get_real_value_traits().to_node_ptr(*replace_this)
                                   , this->priv_header_ptr()
                                   , get_real_value_traits().to_node_ptr(with_this));
      if(safemode_or_autounlink)
         node_algorithms::init(replace_this.pointed_node());
   }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static iterator s_iterator_to(reference value)
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      return iterator (value_traits::to_node_ptr(value), 0);
   }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value) 
   {
      BOOST_STATIC_ASSERT((!stateful_value_traits));
      return const_iterator (value_traits::to_node_ptr(const_cast<reference> (value)), 0);
   }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value)
   {  return iterator (value_traits::to_node_ptr(value), this); }

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //! 
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //! 
   //! <b>Complexity</b>: Constant.
   //! 
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const
   {  return const_iterator (value_traits::to_node_ptr(const_cast<reference> (value)), this); }

   //! <b>Requires</b>: value shall not be in a tree.
   //! 
   //! <b>Effects</b>: init_node puts the hook of a value in a well-known default
   //!   state.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Note</b>: This function puts the hook in the well-known default state
   //!   used by auto_unlink and safe hooks.
   static void init_node(reference value)
   { node_algorithms::init(value_traits::to_node_ptr(value)); }

   //! <b>Effects</b>: Rebalances the tree.
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear.
   void rebalance()
   {  node_algorithms::rebalance(this->priv_header_ptr()); }

   //! <b>Requires</b>: old_root is a node of a tree.
   //! 
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   iterator rebalance_subtree(iterator root)
   {  return iterator(node_algorithms::rebalance_subtree(root.pointed_node()), this); }

   //! <b>Returns</b>: The balance factor (alpha) used in this tree
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant.
   float balance_factor() const
   {  return this->priv_alpha(); }

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
      float old_alpha = this->priv_alpha();
      this->priv_alpha(new_alpha);

      if(new_alpha < old_alpha){
         data_.max_tree_size_ = this->size();
         this->rebalance();
      }
   }
/*
   //! <b>Effects</b>: removes x from a tree of the appropriate type. It has no effect,
   //! if x is not in such a tree. 
   //! 
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Constant time.
   //! 
   //! <b>Note</b>: This static function is only usable with the "safe mode"
   //! hook and non-constant time size lists. Otherwise, the user must use
   //! the non-static "erase(reference )" member. If the user calls
   //! this function with a non "safe mode" or constant time size list
   //! a compilation error will be issued.
   template<class T>
   static void remove_node(T& value)
   {
      //This function is only usable for safe mode hooks and non-constant
      //time lists. 
      //BOOST_STATIC_ASSERT((!(safemode_or_autounlink && constant_time_size)));
      BOOST_STATIC_ASSERT((!constant_time_size));
      BOOST_STATIC_ASSERT((boost::is_convertible<T, value_type>::value));
      node_ptr to_remove(value_traits::to_node_ptr(value));
      node_algorithms::unlink_and_rebalance(to_remove);
      if(safemode_or_autounlink)
         node_algorithms::init(to_remove);
   }
*/

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

   private:
   static sgtree_impl &priv_container_from_end_iterator(const const_iterator &end_iterator)
   {
      header_plus_alpha *r = detail::parent_from_member<header_plus_alpha, node>
         ( boost::intrusive::detail::to_raw_pointer(end_iterator.pointed_node()), &header_plus_alpha::header_);
      node_plus_pred_t *n = detail::parent_from_member
         <node_plus_pred_t, header_plus_alpha>(r, &node_plus_pred_t::header_plus_alpha_);
      data_t *d = detail::parent_from_member<data_t, node_plus_pred_t>(n, &data_t::node_plus_pred_);
      sgtree_impl *scapegoat  = detail::parent_from_member<sgtree_impl, data_t>(d, &sgtree_impl::data_);
      return *scapegoat;
   }

   static sgtree_impl &priv_container_from_iterator(const const_iterator &it)
   {  return priv_container_from_end_iterator(it.end_iterator_from_it());   }
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{  return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
bool operator==
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{
   typedef sgtree_impl<Config> tree_type;
   typedef typename tree_type::const_iterator const_iterator;

   if(tree_type::constant_time_size && x.size() != y.size()){
      return false;
   }
   const_iterator end1 = x.end();
   const_iterator i1 = x.begin();
   const_iterator i2 = y.begin();
   if(tree_type::constant_time_size){
      while (i1 != end1 && *i1 == *i2) {
         ++i1;
         ++i2;
      }
      return i1 == end1;
   }
   else{
      const_iterator end2 = y.end();
      while (i1 != end1 && i2 != end2 && *i1 == *i2) {
         ++i1;
         ++i2;
      }
      return i1 == end1 && i2 == end2;
   }
}

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator!=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{  return !(x == y); }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{  return y < x;  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator<=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{  return !(y < x);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline bool operator>=
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(const sgtree_impl<T, Options...> &x, const sgtree_impl<T, Options...> &y)
#else
(const sgtree_impl<Config> &x, const sgtree_impl<Config> &y)
#endif
{  return !(x < y);  }

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class Config>
#endif
inline void swap
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
(sgtree_impl<T, Options...> &x, sgtree_impl<T, Options...> &y)
#else
(sgtree_impl<Config> &x, sgtree_impl<Config> &y)
#endif
{  x.swap(y);  }

/// @cond
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#else
template<class T, class ...Options>
#endif
struct make_sgtree_opt
{
   typedef typename pack_options
      < sg_set_defaults<T>, 
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type packed_options;
   typedef typename detail::get_value_traits
      <T, typename packed_options::value_traits>::type value_traits;

   typedef sg_setopt
         < value_traits
         , typename packed_options::compare
         , typename packed_options::size_type
         , packed_options::floating_point
         > type;
};
/// @endcond

//! Helper metafunction to define a \c sgtree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = none, class O2 = none
                , class O3 = none, class O4 = none>
#endif
struct make_sgtree
{
   /// @cond
   typedef sgtree_impl
      < typename make_sgtree_opt<T, 
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type
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

   //Assert if passed value traits are compatible with the type
   BOOST_STATIC_ASSERT((detail::is_same<typename real_value_traits::value_type, T>::value));

   sgtree( const value_compare &cmp = value_compare()
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
   {  this->Base::operator=(::boost::move(static_cast<Base&>(x))); return *this;  }

   static sgtree &container_from_end_iterator(iterator end_iterator)
   {  return static_cast<sgtree &>(Base::container_from_end_iterator(end_iterator));   }

   static const sgtree &container_from_end_iterator(const_iterator end_iterator)
   {  return static_cast<const sgtree &>(Base::container_from_end_iterator(end_iterator));   }
};

#endif


} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SGTREE_HPP
