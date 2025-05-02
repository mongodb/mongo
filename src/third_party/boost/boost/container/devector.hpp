//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Benedek Thaler 2015-2016
// (C) Copyright Ion Gaztanaga 2019-2020. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DEVECTOR_HPP
#define BOOST_CONTAINER_DEVECTOR_HPP

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <cstring> // memcpy

#include <boost/assert.hpp>

#include <boost/container/detail/copy_move_algo.hpp>
#include <boost/container/new_allocator.hpp> //new_allocator
#include <boost/container/allocator_traits.hpp> //allocator_traits
#include <boost/container/detail/algorithm.hpp> //equal()
#include <boost/container/throw_exception.hpp>
#include <boost/container/options.hpp>

#include <boost/container/detail/guards_dended.hpp>
#include <boost/container/detail/iterator.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/detail/min_max.hpp>
#include <boost/container/detail/next_capacity.hpp>
#include <boost/container/detail/alloc_helpers.hpp>
#include <boost/container/detail/advanced_insert_int.hpp>

// move
#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
#include <boost/move/detail/fwd_macros.hpp>
#endif
#include <boost/move/detail/move_helpers.hpp>
#include <boost/move/adl_move_swap.hpp>
#include <boost/move/iterator.hpp>
#include <boost/move/traits.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>
#include <boost/move/algo/detail/merge.hpp>

//std
#if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
#include <initializer_list>    //for std::initializer_list
#endif

namespace boost {
namespace container {

#if !defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

struct growth_factor_60;

template<class Options, class AllocatorSizeType>
struct get_devector_opt
{
    typedef devector_opt< typename default_if_void<typename Options::growth_factor_type, growth_factor_60>::type
                        , typename default_if_void<typename Options::stored_size_type, AllocatorSizeType>::type
                        , default_if_zero<Options::free_fraction, relocate_on_90::value>::value
                        > type;
};

template<class AllocatorSizeType>
struct get_devector_opt<void, AllocatorSizeType>
{
    typedef devector_opt< growth_factor_60, AllocatorSizeType, relocate_on_90::value> type;
};

#endif    //#if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

struct reserve_only_tag_t {};
struct reserve_uninitialized_t {};
struct review_implementation_t {};

//struct unsafe_uninitialized_tag_t {};

/**
 * A vector-like sequence container providing front and back operations
 * (e.g: `push_front`/`pop_front`/`push_back`/`pop_back`) with amortized constant complexity.
 *
 * Models the [SequenceContainer], [ReversibleContainer], and [AllocatorAwareContainer] concepts.
 *
 * **Requires**:
 *   - `T` shall be [MoveInsertable] into the devector.
 *   - `T` shall be [Erasable] from any `devector<T, allocator_type, GP>`.
 *   - `GrowthFactor`, and `Allocator` must model the concepts with the same names or be void.
 *
 * **Definition**: `T` is `NothrowConstructible` if it's either nothrow move constructible or
 * nothrow copy constructible.
 *
 * **Definition**: `T` is `NothrowAssignable` if it's either nothrow move assignable or
 * nothrow copy assignable.
 *
 * **Exceptions**: The exception specifications assume `T` is nothrow [Destructible].
 *
 * Most methods providing the strong exception guarantee assume `T` either has a move
 * constructor marked noexcept or is [CopyInsertable] into the devector. If it isn't true,
 * and the move constructor throws, the guarantee is waived and the effects are unspecified.
 *
 * In addition to the exceptions specified in the **Throws** clause, the following operations
 * of `T` can throw when any of the specified concept is required:
 *   - [DefaultInsertable][]: Default constructor
 *   - [MoveInsertable][]: Move constructor
 *   - [CopyInsertable][]: Copy constructor
 *   - [DefaultConstructible][]: Default constructor
 *   - [EmplaceConstructible][]: Constructor selected by the given arguments
 *   - [MoveAssignable][]: Move assignment operator
 *   - [CopyAssignable][]: Copy assignment operator
 *
 * Furthermore, not `noexcept` methods throws whatever the allocator throws
 * if memory allocation fails. Such methods also throw `length_error` if the capacity
 * exceeds `max_size()`.
 *
 * **Remark**: If a method invalidates some iterators, it also invalidates references
 * and pointers to the elements pointed by the invalidated iterators.
 *
 *! \tparam Options A type produced from \c boost::container::devector_options.
 *
 * [SequenceContainer]: http://en.cppreference.com/w/cpp/concept/SequenceContainer
 * [ReversibleContainer]: http://en.cppreference.com/w/cpp/concept/ReversibleContainer
 * [AllocatorAwareContainer]: http://en.cppreference.com/w/cpp/concept/AllocatorAwareContainer
 * [DefaultInsertable]: http://en.cppreference.com/w/cpp/concept/DefaultInsertable
 * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
 * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
 * [Erasable]: http://en.cppreference.com/w/cpp/concept/Erasable
 * [DefaultConstructible]: http://en.cppreference.com/w/cpp/concept/DefaultConstructible
 * [Destructible]: http://en.cppreference.com/w/cpp/concept/Destructible
 * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
 * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
 * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
 */
template < typename T, class A BOOST_CONTAINER_DOCONLY(= void), class Options BOOST_CONTAINER_DOCONLY(= void)>
class devector
{
   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   typedef boost::container::allocator_traits
      <typename real_allocator<T, A>::type>                                      allocator_traits_type;
   typedef typename allocator_traits_type::size_type                             alloc_size_type;
   typedef typename get_devector_opt<Options, alloc_size_type>::type             options_type;
   typedef typename options_type::growth_factor_type                             growth_factor_type;
   typedef typename options_type::stored_size_type                               stored_size_type;
   BOOST_STATIC_CONSTEXPR std::size_t devector_min_free_fraction =
      options_type::free_fraction;

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   public:
   // Standard Interface Types:
   typedef T                                                                     value_type;
   typedef BOOST_CONTAINER_IMPDEF
      (typename real_allocator<T BOOST_MOVE_I A>::type)                          allocator_type;
   typedef allocator_type                                                        stored_allocator_type;
   typedef typename    allocator_traits<allocator_type>::pointer                 pointer;
   typedef typename    allocator_traits<allocator_type>::const_pointer           const_pointer;
   typedef typename    allocator_traits<allocator_type>::reference               reference;
   typedef typename    allocator_traits<allocator_type>::const_reference         const_reference;
   typedef typename    allocator_traits<allocator_type>::size_type               size_type;
   typedef typename    allocator_traits<allocator_type>::difference_type         difference_type;
   typedef pointer                                                               iterator;
   typedef const_pointer                                                         const_iterator;
   typedef BOOST_CONTAINER_IMPDEF
      (boost::container::reverse_iterator<iterator>)                             reverse_iterator;
   typedef BOOST_CONTAINER_IMPDEF
      (boost::container::reverse_iterator<const_iterator>)                       const_reverse_iterator;

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
   private:

   //`allocator_type::value_type` must match container's `value type`. If this
   //assertion fails, please review your allocator definition. 
   BOOST_CONTAINER_STATIC_ASSERT((dtl::is_same<value_type, typename allocator_traits<allocator_type>::value_type>::value));

   BOOST_COPYABLE_AND_MOVABLE(devector)

   // Guard to deallocate buffer on exception
   typedef typename detail::allocation_guard<allocator_type> allocation_guard;

   // Random access pseudo iterator always yielding to the same result
   typedef constant_iterator<T> cvalue_iterator;

   static size_type to_internal_capacity(size_type desired_capacity)
   {
      const size_type rounder = devector_min_free_fraction - 2u;
      const size_type divisor = devector_min_free_fraction - 1u;
      size_type const nc = ((desired_capacity + rounder) / divisor) * devector_min_free_fraction;
      BOOST_ASSERT(desired_capacity <= (nc - nc / devector_min_free_fraction));
      return nc;
   }

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   // Standard Interface
   public:
   // construct/copy/destroy

   /**
   * **Effects**: Constructs an empty devector.
   *
   * **Postcondition**: `empty() && front_free_capacity() == 0
   * && back_free_capacity() == 0`.
   *
   * **Complexity**: Constant.
   */
   devector() BOOST_NOEXCEPT
      : m_()
   {}

   /**
   * **Effects**: Constructs an empty devector, using the specified allocator.
   *
   * **Postcondition**: `empty() && front_free_capacity() == 0
   * && back_free_capacity() == 0`.
   *
   * **Complexity**: Constant.
   */
   explicit devector(const allocator_type& allocator) BOOST_NOEXCEPT
      : m_(allocator)
   {}

   /**
   * **Effects**: Constructs an empty devector, using the specified allocator
   * and reserves `n` slots as if `reserve(n)` was called.
   *
   * **Postcondition**: `empty() && capacity() >= n`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Constant.
   */
   devector(size_type n, reserve_only_tag_t, const allocator_type& allocator = allocator_type())
      : m_(reserve_only_tag_t(), allocator, to_internal_capacity(n))
   {}

   /**
   * **Effects**: Constructs an empty devector, using the specified allocator
   * and reserves at least `front_free_cap + back_free_cap` slots as if `reserve_front(front_cap)` and
   * `reserve_back(back_cap)` was called.
   *
   * **Postcondition**: `empty() && front_free_capacity() >= front_free_cap
   * && back_free_capacity() >= back_free_cap`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Constant.
   */
   devector(size_type front_free_cap, size_type back_free_cap, reserve_only_tag_t, const allocator_type& allocator = allocator_type())
      : m_(reserve_only_tag_t(), allocator, front_free_cap, back_free_cap)
   {}

   /**
   * [DefaultInsertable]: http://en.cppreference.com/w/cpp/concept/DefaultInsertable
   *
   * **Effects**: Constructs a devector with `n` value_initialized elements using the specified allocator.
   *
   * **Requires**: `T` shall be [DefaultInsertable] into `*this`.
   *
   * **Postcondition**: `size() == n`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in `n`.
   */
   explicit devector(size_type n, const allocator_type& allocator = allocator_type())
      : m_(reserve_uninitialized_t(), allocator, n)
   {
      allocation_guard buffer_guard(m_.buffer, m_.capacity, get_allocator_ref());
      boost::container::uninitialized_value_init_alloc_n(get_allocator_ref(), n, this->priv_raw_begin());
      buffer_guard.release();
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * **Effects**: Constructs a devector with `n` default-initialized elements using the specified allocator.
   *
   * **Requires**: `T` shall be [DefaultInsertable] into `*this`.
   *
   * **Postcondition**: `size() == n`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in `n`.
   */
   explicit devector(size_type n, default_init_t, const allocator_type& allocator = allocator_type())
      : m_(reserve_uninitialized_t(), allocator, n)
   {
      allocation_guard buffer_guard(m_.buffer, m_.capacity, get_allocator_ref());
      boost::container::uninitialized_default_init_alloc_n(get_allocator_ref(), n, this->priv_raw_begin());
      buffer_guard.release();
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: Constructs a devector with `n` copies of `value`, using the specified allocator.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Postcondition**: `size() == n`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in `n`.
   */
   devector(size_type n, const T& value, const allocator_type& allocator = allocator_type())
      : m_(reserve_uninitialized_t(), allocator, n)
   {
      construct_from_range(cvalue_iterator(value, n), cvalue_iterator());
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * **Effects**: Constructs a devector equal to the range `[first,last)`, using the specified allocator.
   *
   * **Requires**: `T` shall be [EmplaceConstructible] into `*this` from `*first`. If the specified
   * iterator does not meet the forward iterator requirements, `T` shall also be [MoveInsertable]
   * into `*this`.
   *
   * **Postcondition**: `size() == boost::container::iterator_distance(first, last)
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Makes only `N` calls to the copy constructor of `T` (where `N` is the distance between `first`
   * and `last`), at most one allocation and no reallocations if iterators first and last are of forward,
   * bidirectional, or random access categories. It makes `O(N)` calls to the copy constructor of `T`
   * and `O(log(N)) reallocations if they are just input iterators.
   *
   * **Remarks**: Each iterator in the range `[first,last)` shall be dereferenced exactly once,
   * unless an exception is thrown.
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   */
   template <class InputIterator>
   devector(InputIterator first, InputIterator last, const allocator_type& allocator = allocator_type()
      //Input iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<InputIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_not_input_iterator<InputIterator>
            >::type * = 0)
      )
      : m_(allocator)
   {
      BOOST_CONTAINER_TRY{
         while (first != last) {
            this->emplace_back(*first++);
         }
         BOOST_ASSERT(invariants_ok());
      }
      BOOST_CONTAINER_CATCH(...){
         this->destroy_elements(m_.buffer + m_.front_idx, m_.buffer + m_.back_idx);
         this->deallocate_buffer();
      }
      BOOST_CONTAINER_CATCH_END
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   template <class ForwardIterator>
   devector(ForwardIterator first, ForwardIterator last, const allocator_type& allocator = allocator_type()
      //Other iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<ForwardIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_input_iterator<ForwardIterator>
            >::type * = 0)
      )
      : m_(reserve_uninitialized_t(), allocator, boost::container::iterator_udistance(first, last))
   {
      this->construct_from_range(first, last);
      BOOST_ASSERT(invariants_ok());
   }

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: Copy constructs a devector.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Postcondition**: `this->size() == x.size()`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in the size of `x`.
   */
   devector(const devector& x)
      : m_(reserve_uninitialized_t(), allocator_traits_type::select_on_container_copy_construction(x.get_allocator_ref()), x.size())
   {
      this->construct_from_range(x.begin(), x.end());
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: Copy constructs a devector, using the specified allocator.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Postcondition**: `*this == x`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in the size of `x`.
   */
   devector(const devector& x, const allocator_type& allocator)
      : m_(reserve_uninitialized_t(), allocator, x.size())
   {
      this->construct_from_range(x.begin(), x.end());
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * **Effects**: Moves `rhs`'s resources to `*this`.
   *
   * **Throws**: Nothing.
   *
   * **Postcondition**: *this has the same value `rhs` had before the operation.
   *                    `rhs` is left in an unspecified but valid state.
   *
   * **Exceptions**: Strong exception guarantee if not `noexcept`.
   *
   * **Complexity**: Constant.
   */
   devector(BOOST_RV_REF(devector) rhs) BOOST_NOEXCEPT_OR_NOTHROW
      : m_(::boost::move(rhs.m_))
   {
      BOOST_ASSERT(      invariants_ok());
      BOOST_ASSERT(rhs.invariants_ok());
   }

   /**
   * **Effects**: Moves `rhs`'s resources to `*this`, using the specified allocator.
   *
   * **Throws**: If allocation or T's move constructor throws.
   *
   * **Postcondition**: *this has the same value `rhs` had before the operation.
   *                    `rhs` is left in an unspecified but valid state.
   *
   * **Exceptions**: Strong exception guarantee if not `noexcept`.
   *
   * **Complexity**: Linear if allocator != rhs.get_allocator(), otherwise constant.
   */
   devector(BOOST_RV_REF(devector) rhs, const allocator_type& allocator)
      : m_(review_implementation_t(), allocator, rhs.m_.buffer, rhs.m_.front_idx, rhs.m_.back_idx, rhs.m_.capacity)
   {
      // TODO should move elems-by-elems if the two allocators differ
      // buffer is already acquired, reset rhs
      rhs.m_.capacity = 0u;
      rhs.m_.buffer = pointer();
      rhs.m_.front_idx = 0;
      rhs.m_.back_idx = 0;
      BOOST_ASSERT(      invariants_ok());
      BOOST_ASSERT(rhs.invariants_ok());
   }

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   /**
   * **Equivalent to**: `devector(il.begin(), il.end(), allocator)`.
   */
   devector(const std::initializer_list<T>& il, const allocator_type& allocator = allocator_type())
      : m_(reserve_uninitialized_t(), allocator, il.size())
   {
      this->construct_from_range(il.begin(), il.end());
      BOOST_ASSERT(invariants_ok());
   }
   #endif

/**
   * **Effects**: Destroys the devector. All stored values are destroyed and
   * used memory, if any, deallocated.
   *
   * **Complexity**: Linear in the size of `*this`.
   */
~devector() BOOST_NOEXCEPT
{
   destroy_elements(m_.buffer + m_.front_idx, m_.buffer + m_.back_idx);
   deallocate_buffer();
}

/**
   * **Effects**: Copies elements of `x` to `*this`. Previously
   * held elements get copy assigned to or destroyed.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Postcondition**: `this->size() == x.size()`, the elements of
   * `*this` are copies of elements in `x` in the same order.
   *
   * **Returns**: `*this`.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and the allocator is allowed to be propagated
   * ([propagate_on_container_copy_assignment] is true),
   * Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the size of `x` and `*this`.
   *
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   * [propagate_on_container_copy_assignment]: http://en.cppreference.com/w/cpp/memory/allocator_traits
   */

   inline devector& operator=(BOOST_COPY_ASSIGN_REF(devector) rhs)
   {
      const devector &x = rhs;
      if (this == &x) { return *this; } // skip self

      const bool do_propagate = allocator_traits_type::propagate_on_container_copy_assignment::value;
      BOOST_IF_CONSTEXPR(do_propagate)
      {
         allocator_type &this_alloc = this->get_allocator_ref();
         const allocator_type &other_alloc = x.get_allocator_ref();
         if (this_alloc != other_alloc)
         {
            // new allocator cannot free existing storage
            this->clear();
            this->deallocate_buffer();
            m_.capacity = 0u;
            m_.buffer = pointer();
         }
         dtl::bool_<do_propagate> flag;
         dtl::assign_alloc(this_alloc, other_alloc, flag);
      }

      size_type n = x.size();
      if (m_.capacity >= n)
      {
            this->overwrite_buffer(x.begin(), x.end());
      }
      else
      {
            this->allocate_and_copy_range(x.begin(), x.end());
      }

      BOOST_ASSERT(invariants_ok());

      return *this;
   }

   /**
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   *
   * **Effects**: Moves elements of `x` to `*this`. Previously
   * held elements get move/copy assigned to or destroyed.
   *
   * **Requires**: `T` shall be [MoveInsertable] into `*this`.
   *
   * **Postcondition**: `x` is left in an unspecified but valid state.
   *
   * **Returns**: `*this`.
   *
   * **Exceptions**: Basic exception guarantee if not `noexcept`.
   *
   * **Complexity**: Constant if allocator_traits_type::
   *   propagate_on_container_move_assignment is true or
   *   this->get>allocator() == x.get_allocator(). Linear otherwise.
   */
   devector& operator=(BOOST_RV_REF(devector) x)
      BOOST_NOEXCEPT_IF(allocator_traits_type::propagate_on_container_move_assignment::value
                                 || allocator_traits_type::is_always_equal::value)
   {
      if (BOOST_LIKELY(this != &x)) {
         //We know resources can be transferred at comiple time if both allocators are
         //always equal or the allocator is going to be propagated
         const bool can_steal_resources_alloc
            = allocator_traits_type::propagate_on_container_move_assignment::value
            || allocator_traits_type::is_always_equal::value;
         dtl::bool_<can_steal_resources_alloc> flag;
         this->priv_move_assign(boost::move(x), flag);
      }
      BOOST_ASSERT(invariants_ok());
      return *this;
   }

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   /**
   * **Effects**: Copies elements of `il` to `*this`. Previously
   * held elements get copy assigned to or destroyed.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this` and [CopyAssignable].
   *
   * **Postcondition**: `this->size() == il.size()`, the elements of
   * `*this` are copies of elements in `il` in the same order.
   *
   * **Exceptions**: Strong exception guarantee if `T` is nothrow copy assignable
   * from `T` and `NothrowConstructible`, Basic exception guarantee otherwise.
   *
   * **Returns**: `*this`.
   *
   * **Complexity**: Linear in the size of `il` and `*this`.
   *
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
   */
   inline devector& operator=(std::initializer_list<T> il)
   {
      this->assign(il.begin(), il.end());
      return *this;
   }
   #endif

   /**
   * **Effects**: Replaces elements of `*this` with a copy of `[first,last)`.
   * Previously held elements get copy assigned to or destroyed.
   *
   * **Requires**: `T` shall be [EmplaceConstructible] from `*first`. If the specified iterator
   * does not meet the forward iterator requirements, `T` shall be also [MoveInsertable] into `*this`.
   *
   * **Precondition**: `first` and `last` are not iterators into `*this`.
   *
   * **Postcondition**: `size() == N`, where `N` is the distance between `first` and `last`.
   *
   * **Exceptions**: Strong exception guarantee if `T` is nothrow copy assignable
   * from `*first` and `NothrowConstructible`, Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the distance between `first` and `last`.
   * Makes a single reallocation at most if the iterators `first` and `last`
   * are of forward, bidirectional, or random access categories. It makes
   * `O(log(N))` reallocations if they are just input iterators.
   *
   * **Remarks**: Each iterator in the range `[first,last)` shall be dereferenced exactly once,
   * unless an exception is thrown.
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   */
   template <class InputIterator>
   void assign(InputIterator first, InputIterator last
      //Input iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<InputIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_not_input_iterator<InputIterator>
            >::type * = 0)
      )
   {
      first = overwrite_buffer_impl(first, last, dtl::false_());
      while (first != last)
      {
         this->emplace_back(*first++);
      }
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   template <class ForwardIterator>
   void assign(ForwardIterator first, ForwardIterator last
      //Other iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<ForwardIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_input_iterator<ForwardIterator>
            >::type * = 0)
      )
   {
      const size_type n = boost::container::iterator_udistance(first, last);

      if (m_.capacity >= n)
      {
         overwrite_buffer(first, last);
      }
      else
      {
         allocate_and_copy_range(first, last);
      }

      BOOST_ASSERT(invariants_ok());
   }

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   /**
    * **Effects**: Replaces elements of `*this` with `n` copies of `u`.
    * Previously held elements get copy assigned to or destroyed.
    *
    * **Requires**: `T` shall be [CopyInsertable] into `*this` and
    * [CopyAssignable].
    *
    * **Precondition**: `u` is not a reference into `*this`.
    *
    * **Postcondition**: `size() == n` and the elements of
    * `*this` are copies of `u`.
    *
    * **Exceptions**: Strong exception guarantee if `T` is nothrow copy assignable
    * from `u` and `NothrowConstructible`, Basic exception guarantee otherwise.
    *
    * **Complexity**: Linear in `n` and the size of `*this`.
    *
    * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
    * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
    */
   inline void assign(size_type n, const T& u)
   {
      cvalue_iterator first(u, n);
      cvalue_iterator last;
      this->assign(first, last);
   }

    #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
    /** **Equivalent to**: `assign(il.begin(), il.end())`. */
   inline void assign(std::initializer_list<T> il)
    {
         this->assign(il.begin(), il.end());
    }
    #endif

   /**
    * **Returns**: A copy of the allocator associated with the container.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    allocator_type get_allocator() const BOOST_NOEXCEPT
   {
      return static_cast<const allocator_type&>(m_);
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const allocator_type &get_stored_allocator() const BOOST_NOEXCEPT
   {
      return static_cast<const allocator_type&>(m_);
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
         allocator_type &get_stored_allocator() BOOST_NOEXCEPT
   {
      return static_cast<allocator_type&>(m_);
   }

   // iterators

   /**
    * **Returns**: A iterator pointing to the first element in the devector,
    * or the past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
         iterator begin() BOOST_NOEXCEPT
   {
      return m_.buffer + m_.front_idx;
   }

   /**
    * **Returns**: A constant iterator pointing to the first element in the devector,
    * or the past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_iterator begin() const BOOST_NOEXCEPT
   {
      return m_.buffer + m_.front_idx;
   }

   /**
    * **Returns**: An iterator pointing past the last element of the container.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
         iterator end() BOOST_NOEXCEPT
   {
      return m_.buffer + m_.back_idx;
   }

   /**
    * **Returns**: A constant iterator pointing past the last element of the container.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_iterator end() const BOOST_NOEXCEPT
   {
      return m_.buffer + m_.back_idx;
   }

   /**
    * **Returns**: A reverse iterator pointing to the first element in the reversed devector,
    * or the reverse past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    reverse_iterator rbegin() BOOST_NOEXCEPT
   {
      return reverse_iterator(m_.buffer + m_.back_idx);
   }

   /**
    * **Returns**: A constant reverse iterator
    * pointing to the first element in the reversed devector,
    * or the reverse past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_reverse_iterator rbegin() const BOOST_NOEXCEPT
   {
      return const_reverse_iterator(m_.buffer + m_.back_idx);
   }

   /**
    * **Returns**: A reverse iterator pointing past the last element in the
    * reversed container, or to the beginning of the reversed container if it's empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    reverse_iterator rend() BOOST_NOEXCEPT
   {
      return reverse_iterator(m_.buffer + m_.front_idx);
   }

   /**
    * **Returns**: A constant reverse iterator pointing past the last element in the
    * reversed container, or to the beginning of the reversed container if it's empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_reverse_iterator rend() const BOOST_NOEXCEPT
   {
      return const_reverse_iterator(m_.buffer + m_.front_idx);
   }

   /**
    * **Returns**: A constant iterator pointing to the first element in the devector,
    * or the past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_iterator cbegin() const BOOST_NOEXCEPT
   {
      return m_.buffer + m_.front_idx;
   }

   /**
    * **Returns**: A constant iterator pointing past the last element of the container.
    *
    * **Complexity**: Constant.
    */
   const_iterator cend() const BOOST_NOEXCEPT
   {
      return m_.buffer + m_.back_idx;
   }

   /**
    * **Returns**: A constant reverse iterator
    * pointing to the first element in the reversed devector,
    * or the reverse past the end iterator if the devector is empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_reverse_iterator crbegin() const BOOST_NOEXCEPT
   {
      return const_reverse_iterator(m_.buffer + m_.back_idx);
   }

   /**
    * **Returns**: A constant reverse iterator pointing past the last element in the
    * reversed container, or to the beginning of the reversed container if it's empty.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    const_reverse_iterator crend() const BOOST_NOEXCEPT
   {
      return const_reverse_iterator(m_.buffer + m_.front_idx);
   }

   // capacity

   /**
    * **Returns**: True, if `size() == 0`, false otherwise.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    bool empty() const BOOST_NOEXCEPT
   {
      return m_.front_idx == m_.back_idx;
   }

   /**
    * **Returns**: The number of elements the devector contains.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    size_type size() const BOOST_NOEXCEPT
   {
      return size_type(m_.back_idx - m_.front_idx);
   }

   /**
    * **Returns**: The maximum number of elements the devector could possibly hold.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    size_type max_size() const BOOST_NOEXCEPT
   {
      size_type alloc_max = allocator_traits_type::max_size(get_allocator_ref());
      size_type size_type_max = (size_type)-1;
      return (alloc_max <= size_type_max) ? size_type(alloc_max) : size_type_max;
   }

   /**
   * **Returns**: The *minimum* number of elements that can be inserted into devector using
   *   position-based insertions without requiring a reallocation. Note that, unlike in 
   *   typical sequence containers like `vector`, `capacity()`, `capacity()` can be smaller than `size()`.
   *   This can happen if a user inserts elements in a particular way (usually inserting at
   *   front up to front_free_capacity() and at back up to back_free_capacity()).
   * 
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   size_type capacity() const BOOST_NOEXCEPT
   {
      size_type const cap_reserve = m_.capacity/devector_min_free_fraction;
      return m_.capacity > cap_reserve ? (m_.capacity - cap_reserve) : 0u;
   }

   /**
    * **Returns**: The total number of elements that can be pushed to the front of the
    * devector without requiring reallocation.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   size_type front_free_capacity() const BOOST_NOEXCEPT
   {
      return m_.front_idx;
   }

   /**
    * **Returns**: The total number of elements that can be pushed to the back of the
    * devector without requiring reallocation.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
    size_type back_free_capacity() const BOOST_NOEXCEPT
   {
      return size_type(m_.capacity - m_.back_idx);
   }

   /**
   * **Effects**: If `sz` is greater than the size of `*this`,
   * additional value-initialized elements are inserted. Invalidates iterators
   * if reallocation is needed. If `sz` is smaller than than the size of `*this`,
   * elements are erased from the extremes.
   *
   * **Requires**: T shall be [MoveInsertable] into *this and [DefaultConstructible].
   *
   * **Postcondition**: `sz == size()`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in the size of `*this` and `sz`.
   *
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   * [DefaultConstructible]: http://en.cppreference.com/w/cpp/concept/DefaultConstructible
   */
   inline void resize(size_type sz)
   {
      this->resize_back(sz);
   }

   /**
    * **Effects**: Same as resize(sz) but creates default-initialized
    * value-initialized.
    */
   inline void resize(size_type sz, default_init_t)
   {
      this->resize_back(sz, default_init);
   }

   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: If `sz` is greater than the size of `*this`,
   * copies of `c` are inserted at extremes.
   * If `sz` is smaller than than the size of `*this`,
   * elements are popped from the extremes.
   *
   * **Postcondition**: `sz == size()`.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Linear in the size of `*this` and `sz`.
   */
   inline void resize(size_type sz, const T& c)
   {
      this->resize_back(sz, c);
   }

   /**
    * **Effects**: If `sz` is greater than the size of `*this`,
    * additional value-initialized elements are inserted
    * to the front. Invalidates iterators if reallocation is needed.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the front.
    *
    * **Requires**: T shall be [MoveInsertable] into *this and [DefaultConstructible].
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    *
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    * [DefaultConstructible]: http://en.cppreference.com/w/cpp/concept/DefaultConstructible
    */
   inline void resize_front(size_type sz)
   {
      resize_front_impl(sz);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * **Effects**: If `sz` is greater than the size of `*this`,
    * additional value-initialized elements are inserted
    * to the front. Invalidates iterators if reallocation is needed.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the front.
    *
    * **Requires**: T shall be [MoveInsertable] into *this and default_initializable.
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    *
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    */
   inline void resize_front(size_type sz, default_init_t)
   {
      resize_front_impl(sz, default_init);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
    *
    * **Effects**: If `sz` is greater than the size of `*this`,
    * copies of `c` are inserted to the front.
    * Invalidates iterators if reallocation is needed.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the front.
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Requires**: `T` shall be [CopyInsertable] into `*this`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    */
   inline void resize_front(size_type sz, const T& c)
   {
      resize_front_impl(sz, c);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * **Effects**: If `sz` is greater than the size of `*this`,
    * additional value-initialized elements are inserted
    * to the back. Invalidates iterators if reallocation is needed.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the back.
    *
    * **Requires**: T shall be [MoveInsertable] into *this and [DefaultConstructible].
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    *
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    * [DefaultConstructible]: http://en.cppreference.com/w/cpp/concept/DefaultConstructible
    */
   inline void resize_back(size_type sz)
   {
      resize_back_impl(sz);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * **Effects**: If `sz` is greater than the size of `*this`,
    * additional value-initialized elements are inserted
    * to the back. Invalidates iterators if reallocation is needed.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the back.
    *
    * **Requires**: T shall be [MoveInsertable] into *this and default initializable.
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    *
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    */
   inline void resize_back(size_type sz, default_init_t)
   {
      resize_back_impl(sz, default_init);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
    *
    * **Effects**: If `sz` is greater than the size of `*this`,
    * copies of `c` are inserted to the back.
    * If `sz` is smaller than than the size of `*this`,
    * elements are popped from the back.
    *
    * **Postcondition**: `sz == size()`.
    *
    * **Requires**: `T` shall be [CopyInsertable] into `*this`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of `*this` and `sz`.
    */
   inline void resize_back(size_type sz, const T& c)
   {
      resize_back_impl(sz, c);
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    *
    * **Effects**: Ensures that at least `n` elements can be inserted
    * without requiring reallocation, where `n` is `new_capacity - size()`,
    * if `n` is positive. Otherwise, there are no effects.
    * Invalidates iterators if reallocation is needed.
    *
    * **Requires**: `T` shall be [MoveInsertable] into `*this`.
    *
    * **Complexity**: Linear in the size of *this.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Throws**: length_error if `new_capacity > max_size()`.
    */
   inline void reserve(size_type new_capacity)
   {
      if (this->capacity() < new_capacity) {
         const size_type rounder = devector_min_free_fraction - 2u;
         const size_type divisor = devector_min_free_fraction - 1u;
         size_type const nc = ((new_capacity + rounder)/divisor)*devector_min_free_fraction;
         BOOST_ASSERT(new_capacity <= (nc - nc / devector_min_free_fraction));
         size_type const sz = this->size();
         reallocate_at(nc, (nc-sz)/2u);
      }
      BOOST_ASSERT(invariants_ok());
   }

   /**
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    *
    * **Effects**: Ensures that `n` elements can be pushed to the front
    * without requiring reallocation, where `n` is `new_capacity - size()`,
    * if `n` is positive. Otherwise, there are no effects.
    * Invalidates iterators if reallocation is needed.
    *
    * **Requires**: `T` shall be [MoveInsertable] into `*this`.
    *
    * **Complexity**: Linear in the size of *this.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Throws**: `length_error` if `new_capacity > max_size()`.
    */
   inline void reserve_front(size_type new_capacity)
   {
      if (front_capacity() >= new_capacity) { return; }

      reallocate_at(new_capacity + back_free_capacity(), new_capacity - size());

      BOOST_ASSERT(invariants_ok());
   }

   /**
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    *
    * **Effects**: Ensures that `n` elements can be pushed to the back
    * without requiring reallocation, where `n` is `new_capacity - size()`,
    * if `n` is positive. Otherwise, there are no effects.
    * Invalidates iterators if reallocation is needed.
    *
    * **Requires**: `T` shall be [MoveInsertable] into `*this`.
    *
    * **Complexity**: Linear in the size of *this.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Throws**: length_error if `new_capacity > max_size()`.
    */
   inline void reserve_back(size_type new_capacity)
   {
      if (back_capacity() >= new_capacity) { return; }

      reallocate_at(new_capacity + front_free_capacity(), m_.front_idx);

      BOOST_ASSERT(invariants_ok());
   }


   /**
    * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
    *
    * **Effects**: Reduces `capacity()` to `size()`. Invalidates iterators.
    *
    * **Requires**: `T` shall be [MoveInsertable] into `*this`.
    *
    * **Exceptions**: Strong exception guarantee.
    *
    * **Complexity**: Linear in the size of *this.
    */
   inline void shrink_to_fit()
   {
      if(this->front_capacity() || this->back_capacity())
            this->reallocate_at(size(), 0);
   }

   // element access:

   /**
    * **Returns**: A reference to the `n`th element in the devector.
    *
    * **Precondition**: `n < size()`.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   reference operator[](size_type n) BOOST_NOEXCEPT
   {
      BOOST_ASSERT(n < size());
      return m_.buffer[m_.front_idx + n];
   }

   /**
    * **Returns**: A constant reference to the `n`th element in the devector.
    *
    * **Precondition**: `n < size()`.
    *
    * **Complexity**: Constant.
    */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   const_reference operator[](size_type n) const BOOST_NOEXCEPT
   {
      BOOST_ASSERT(n < size());
      return m_.buffer[m_.front_idx + n];
   }

   //! <b>Requires</b>: size() >= n.
   //!
   //! <b>Effects</b>: Returns an iterator to the nth element
   //!   from the beginning of the container. Returns end()
   //!   if n == size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      iterator nth(size_type n) BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(n <= size());
      return iterator(m_.buffer + (m_.front_idx + n));
   }

   //! <b>Requires</b>: size() >= n.
   //!
   //! <b>Effects</b>: Returns a const_iterator to the nth element
   //!   from the beginning of the container. Returns end()
   //!   if n == size().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_iterator nth(size_type n) const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(n <= size());
      return const_iterator(m_.buffer + (m_.front_idx + n));
   }

   //! <b>Requires</b>: begin() <= p <= end().
   //!
   //! <b>Effects</b>: Returns the index of the element pointed by p
   //!   and size() if p == end().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type index_of(iterator p) BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(p >= begin());
      BOOST_ASSERT(p <= end());
      return static_cast<size_type>(p - this->begin());
   }

   //! <b>Requires</b>: begin() <= p <= end().
   //!
   //! <b>Effects</b>: Returns the index of the element pointed by p
   //!   and size() if p == end().
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Non-standard extension
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      size_type index_of(const_iterator p) const BOOST_NOEXCEPT_OR_NOTHROW
   {
      BOOST_ASSERT(p >= cbegin());
      BOOST_ASSERT(p <= cend());
      return static_cast<size_type>(p - this->cbegin());
   }

   /**
   * **Returns**: A reference to the `n`th element in the devector.
   *
   * **Throws**: `out_of_range`, if `n >= size()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reference at(size_type n)
   {
      if (size() <= n)
            throw_out_of_range("devector::at out of range");
      return (*this)[n];
   }

   /**
   * **Returns**: A constant reference to the `n`th element in the devector.
   *
   * **Throws**: `out_of_range`, if `n >= size()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reference at(size_type n) const
   {
      if (size() <= n)
            throw_out_of_range("devector::at out of range");
      return (*this)[n];
   }

   /**
   * **Returns**: A reference to the first element in the devector.
   *
   * **Precondition**: `!empty()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reference front() BOOST_NOEXCEPT
   {
      BOOST_ASSERT(!empty());

      return m_.buffer[m_.front_idx];
   }

   /**
   * **Returns**: A constant reference to the first element in the devector.
   *
   * **Precondition**: `!empty()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reference front() const BOOST_NOEXCEPT
   {
      BOOST_ASSERT(!empty());

      return m_.buffer[m_.front_idx];
   }

   /**
   * **Returns**: A reference to the last element in the devector.
   *
   * **Precondition**: `!empty()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      reference back() BOOST_NOEXCEPT
   {
      BOOST_ASSERT(!empty());

      return m_.buffer[m_.back_idx - 1u];
   }

   /**
   * **Returns**: A constant reference to the last element in the devector.
   *
   * **Precondition**: `!empty()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const_reference back() const BOOST_NOEXCEPT
   {
      BOOST_ASSERT(!empty());

      return m_.buffer[m_.back_idx - 1u];
   }

   /**
   * **Returns**: A pointer to the underlying array serving as element storage.
   * The range `[data(); data() + size())` is always valid. For a non-empty devector,
   * `data() == &front()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      T* data() BOOST_NOEXCEPT
   {
      return boost::movelib::to_raw_pointer(m_.buffer) + m_.front_idx;
   }

   /**
   * **Returns**: A constant pointer to the underlying array serving as element storage.
   * The range `[data(); data() + size())` is always valid. For a non-empty devector,
   * `data() == &front()`.
   *
   * **Complexity**: Constant.
   */
   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      const T* data() const BOOST_NOEXCEPT
   {
      return boost::movelib::to_raw_pointer(m_.buffer) + m_.front_idx;
   }

   // modifiers:

   /**
   * **Effects**: Pushes a new element to the front of the devector.
   * The element is constructed in-place, using the perfect forwarded `args`
   * as constructor arguments. Invalidates iterators if reallocation is needed.
   * (`front_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [EmplaceConstructible] from `args` and [MoveInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `front_free_capacity() > 0`)
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   */
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class... Args>
   reference emplace_front(Args&&... args)
   {
      if (BOOST_LIKELY(front_free_capacity() != 0)) // fast path
      {
         pointer const p = m_.buffer + (m_.front_idx - 1u);
         this->alloc_construct(p, boost::forward<Args>(args)...);
         --m_.front_idx;
         BOOST_ASSERT(invariants_ok());
         return *p;
      }
      else
      {
         typedef dtl::insert_emplace_proxy<allocator_type, Args...> proxy_t;
         return *this->insert_range_slow_path(this->begin(), 1, proxy_t(::boost::forward<Args>(args)...));
      }
   }

   #else //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_CONTAINER_DEVECTOR_EMPLACE_FRONT(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline reference emplace_front(BOOST_MOVE_UREF##N)\
   {\
      if (front_free_capacity())\
      {\
         pointer const p = m_.buffer + (m_.front_idx - 1u);\
         this->alloc_construct(p BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         --m_.front_idx;\
         return *p;\
      }\
      else\
      {\
         typedef dtl::insert_emplace_proxy_arg##N<allocator_type BOOST_MOVE_I##N BOOST_MOVE_TARG##N> proxy_t;\
         return *this->insert_range_slow_path(this->begin(), 1, proxy_t(BOOST_MOVE_FWD##N));\
      }\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_DEVECTOR_EMPLACE_FRONT)
   #undef BOOST_CONTAINER_DEVECTOR_EMPLACE_FRONT

   #endif

   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: Pushes the copy of `x` to the front of the devector.
   * Invalidates iterators if reallocation is needed.
   * (`front_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `front_free_capacity() > 0`)
   */
   void push_front(const T& x);

   /**
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   *
   * **Effects**: Move constructs a new element at the front of the devector using `x`.
   * Invalidates iterators if reallocation is needed.
   * (`front_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [MoveInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee, not regarding the state of `x`.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `front_free_capacity() > 0`)
   */
   void push_front(T&& x);

   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_front, T, void, priv_push_front)
   #endif

   /**
   * **Effects**: Removes the first element of `*this`.
   *
   * **Precondition**: `!empty()`.
   *
   * **Postcondition**: `front_free_capacity()` is incremented by 1.
   *
   * **Complexity**: Constant.
   */
   void pop_front() BOOST_NOEXCEPT
   {
      BOOST_ASSERT(!empty());
      allocator_traits_type::destroy(get_allocator_ref(), boost::movelib::to_raw_pointer(m_.buffer + m_.front_idx));
      ++m_.front_idx;
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * **Effects**: Pushes a new element to the back of the devector.
   * The element is constructed in-place, using the perfect forwarded `args`
   * as constructor arguments. Invalidates iterators if reallocation is needed.
   * (`back_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [EmplaceConstructible] from `args` and [MoveInsertable] into `*this`,
   * and [MoveAssignable].
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `back_free_capacity() > 0`)
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   */
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class... Args>
   inline reference emplace_back(Args&&... args)
   {
      if (BOOST_LIKELY(this->back_free_capacity() != 0)){
         pointer const p = m_.buffer + m_.back_idx;
         this->alloc_construct(p, boost::forward<Args>(args)...);
         ++m_.back_idx;
         BOOST_ASSERT(invariants_ok());
         return *p;
      }
      else {
         typedef dtl::insert_emplace_proxy<allocator_type, Args...> proxy_t;
         return *this->insert_range_slow_path(this->end(), 1, proxy_t(::boost::forward<Args>(args)...));
      }
   }

   #else //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_CONTAINER_DEVECTOR_EMPLACE_BACK(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline reference emplace_back(BOOST_MOVE_UREF##N)\
   {\
      if (this->back_free_capacity()){\
         pointer const p = m_.buffer + m_.back_idx;\
         this->alloc_construct(p BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         ++m_.back_idx;\
         return *p;\
      }\
      else {\
         typedef dtl::insert_emplace_proxy_arg##N<allocator_type BOOST_MOVE_I##N BOOST_MOVE_TARG##N> proxy_t;\
         return *this->insert_range_slow_path(this->end(), 1, proxy_t(BOOST_MOVE_FWD##N));\
      }\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_DEVECTOR_EMPLACE_BACK)
   #undef BOOST_CONTAINER_DEVECTOR_EMPLACE_BACK

   #endif    //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)


   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   /**
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   *
   * **Effects**: Pushes the copy of `x` to the back of the devector.
   * Invalidates iterators if reallocation is needed.
   * (`back_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `back_free_capacity() > 0`)
   */
   void push_back(const T& x);

   /**
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   *
   * **Effects**: Move constructs a new element at the back of the devector using `x`.
   * Invalidates iterators if reallocation is needed.
   * (`back_free_capacity() == 0`)
   *
   * **Requires**: `T` shall be [MoveInsertable] into `*this`.
   *
   * **Exceptions**: Strong exception guarantee, not regarding the state of `x`.
   *
   * **Complexity**: Amortized constant in the size of `*this`.
   * (Constant, if `back_free_capacity() > 0`)
   */
   void push_back(T&& x);

   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH(push_back, T, void, priv_push_back)
   #endif

   /**
   * **Effects**: Removes the last element of `*this`.
   *
   * **Precondition**: `!empty()`.
   *
   * **Postcondition**: `back_free_capacity()` is incremented by 1.
   *
   * **Complexity**: Constant.
   */
   void pop_back() BOOST_NOEXCEPT
   {
      BOOST_ASSERT(! empty());
      --m_.back_idx;
      allocator_traits_type::destroy(get_allocator_ref(), boost::movelib::to_raw_pointer(m_.buffer + m_.back_idx));
      BOOST_ASSERT(invariants_ok());
   }

   /**
   * **Effects**: Constructs a new element before the element pointed by `position`.
   * The element is constructed in-place, using the perfect forwarded `args`
   * as constructor arguments. Invalidates iterators if reallocation is needed.
   *
   * **Requires**: `T` shall be [EmplaceConstructible], and [MoveInsertable] into `*this`,
   * and [MoveAssignable].
   *
   * **Returns**: Iterator pointing to the newly constructed element.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and `NothrowAssignable`, Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the size of `*this`.
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   */
   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <class... Args>
   iterator emplace(const_iterator position, Args&&... args)
   {
      BOOST_ASSERT(position >= begin());
      BOOST_ASSERT(position <= end());
      typedef dtl::insert_emplace_proxy<allocator_type, Args...> proxy_t;
      bool prefer_move_back;
      if (position == end()){
         if(back_free_capacity()) // fast path
         {
            pointer const p = m_.buffer + m_.back_idx;
            this->alloc_construct(p, boost::forward<Args>(args)...);
            ++m_.back_idx;
            return iterator(p);
         }
         prefer_move_back = true;
      }
      else if (position == begin()){
         if(front_free_capacity()) // secondary fast path
         {
            pointer const p = m_.buffer + (m_.front_idx - 1);
            this->alloc_construct(p, boost::forward<Args>(args)...);
            --m_.front_idx;
            return iterator(p);
         }
         prefer_move_back = false;
      }
      else{
         iterator nonconst_pos = unconst_iterator(position);
         prefer_move_back = should_move_back(position);

         if(prefer_move_back){
            if(back_free_capacity()){
               boost::container::expand_forward_and_insert_nonempty_middle_alloc
                  ( get_allocator_ref()
                  , boost::movelib::to_raw_pointer(nonconst_pos)
                  , this->priv_raw_end()
                  , 1, proxy_t(::boost::forward<Args>(args)...));
               ++m_.back_idx;
               return nonconst_pos;
            }
         }
         else{
            if (front_free_capacity()){
               boost::container::expand_backward_and_insert_nonempty_middle_alloc
               (get_allocator_ref()
                  , this->priv_raw_begin()
                  , boost::movelib::to_raw_pointer(nonconst_pos)
                  , 1, proxy_t(::boost::forward<Args>(args)...));
               --m_.front_idx;
               return --nonconst_pos;
            }
         }
      }
      return this->insert_range_slow_path(position, 1, proxy_t(::boost::forward<Args>(args)...));
   }

   #else //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_CONTAINER_DEVECTOR_EMPLACE(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   iterator emplace(const_iterator position BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      BOOST_ASSERT(position >= begin());\
      BOOST_ASSERT(position <= end());\
      typedef dtl::insert_emplace_proxy_arg##N<allocator_type  BOOST_MOVE_I##N BOOST_MOVE_TARG##N> proxy_t;\
      bool prefer_move_back;\
      if (position == end()){\
         if(back_free_capacity())\
         {\
            pointer const p = m_.buffer + m_.back_idx;\
            this->alloc_construct(p BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
            ++m_.back_idx;\
            return iterator(p);\
         }\
         prefer_move_back = true;\
      }\
      else if (position == begin()){\
         if(front_free_capacity())\
         {\
            pointer const p = m_.buffer + (m_.front_idx - 1);\
            this->alloc_construct(p BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
            --m_.front_idx;\
            return iterator(p);\
         }\
         prefer_move_back = false;\
      }\
      else{\
         iterator nonconst_pos = unconst_iterator(position);\
         prefer_move_back = should_move_back(position);\
         \
         if(prefer_move_back){\
            if(back_free_capacity()){\
               boost::container::expand_forward_and_insert_nonempty_middle_alloc\
                  ( get_allocator_ref()\
                  , boost::movelib::to_raw_pointer(nonconst_pos)\
                  , this->priv_raw_end()\
                  , 1, proxy_t(BOOST_MOVE_FWD##N));\
               ++m_.back_idx;\
               return nonconst_pos;\
            }\
         }\
         else{\
            if (front_free_capacity()){\
               boost::container::expand_backward_and_insert_nonempty_middle_alloc\
               (get_allocator_ref()\
                  , this->priv_raw_begin()\
                  , boost::movelib::to_raw_pointer(nonconst_pos)\
                  , 1, proxy_t(BOOST_MOVE_FWD##N));\
               --m_.front_idx;\
               return --nonconst_pos;\
            }\
         }\
      }\
      return this->insert_range_slow_path(position, 1, proxy_t(BOOST_MOVE_FWD##N));\
   }\
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_DEVECTOR_EMPLACE)
   #undef BOOST_CONTAINER_DEVECTOR_EMPLACE

   #endif    //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)


   #if defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   /**
   * **Effects**: Copy constructs a new element before the element pointed by `position`,
   * using `x` as constructor argument. Invalidates iterators if reallocation is needed.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this` and and [CopyAssignable].
   *
   * **Returns**: Iterator pointing to the newly constructed element.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and `NothrowAssignable`, Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the size of `*this`.
   *
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
   */
   iterator insert(const_iterator position, const T &x);

   /**
   * **Effects**: Move constructs a new element before the element pointed by `position`,
   * using `x` as constructor argument. Invalidates iterators if reallocation is needed.
   *
   * **Requires**: `T` shall be [MoveInsertable] into `*this` and and [CopyAssignable].
   *
   * **Returns**: Iterator pointing to the newly constructed element.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and `NothrowAssignable` (not regarding the state of `x`),
   * Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the size of `*this`.
   *
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
   */
   iterator insert(const_iterator position, T &&x);
   #else
   BOOST_MOVE_CONVERSION_AWARE_CATCH_1ARG(insert, T, iterator, priv_insert, const_iterator, const_iterator)
   #endif

   /**
   * **Effects**: Copy constructs `n` elements before the element pointed by `position`,
   * using `x` as constructor argument. Invalidates iterators if reallocation is needed.
   *
   * **Requires**: `T` shall be [CopyInsertable] into `*this` and and [CopyAssignable].
   *
   * **Returns**: Iterator pointing to the first inserted element, or `position`, if `n` is zero.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and `NothrowAssignable`, Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in the size of `*this` and `n`.
   *
   * [CopyInsertable]: http://en.cppreference.com/w/cpp/concept/CopyInsertable
   * [CopyAssignable]: http://en.cppreference.com/w/cpp/concept/CopyAssignable
   */
   inline iterator insert(const_iterator position, size_type n, const T& x)
   {
      cvalue_iterator first(x, n);
      cvalue_iterator last = first + n;
      return this->insert_range(position, first, last);
   }

   /**
   * **Effects**: Copy constructs elements before the element pointed by position
   * using each element in the range pointed by `first` and `last` as constructor arguments.
   * Invalidates iterators if reallocation is needed.
   *
   * **Requires**: `T` shall be [EmplaceConstructible] into `*this` from `*first`. If the specified iterator
   * does not meet the forward iterator requirements, `T` shall also be [MoveInsertable] into `*this`
   * and [MoveAssignable].
   *
   * **Precondition**: `first` and `last` are not iterators into `*this`.
   *
   * **Returns**: Iterator pointing to the first inserted element, or `position`, if `first == last`.
   *
   * **Complexity**: Linear in the size of `*this` and `N` (where `N` is the distance between `first` and `last`).
   * Makes only `N` calls to the constructor of `T` and no reallocations if iterators `first` and `last`
   * are of forward, bidirectional, or random access categories. It makes 2N calls to the copy constructor of `T`
   * and `O(log(N)) reallocations if they are just input iterators.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowConstructible`
   * and `NothrowAssignable`, Basic exception guarantee otherwise.
   *
   * **Remarks**: Each iterator in the range `[first,last)` shall be dereferenced exactly once,
   * unless an exception is thrown.
   *
   * [EmplaceConstructible]: http://en.cppreference.com/w/cpp/concept/EmplaceConstructible
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   */
   template <class InputIterator>
   iterator insert(const_iterator position, InputIterator first, InputIterator last
      //Input iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<InputIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_not_input_iterator<InputIterator>
            >::type * = 0)
      )
   {
      if (position == end())
      {
         size_type insert_index = size();

         for (; first != last; ++first)
         {
            this->emplace_back(*first);
         }

         return begin() + insert_index;
      }
      else
      {
         const size_type insert_index = static_cast<size_type>(position - this->cbegin());
         const size_type old_size = static_cast<size_type>(this->size());

         for (; first != last; ++first) {
            this->emplace_back(*first);
         }
         iterator rit (this->begin() + insert_index);
         boost::movelib::rotate_gcd(rit, this->begin() + old_size, this->begin() + this->size());
         return rit;
      }
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   template <class ForwardIterator>
   inline iterator insert(const_iterator position, ForwardIterator first, ForwardIterator last
      //Other iterators
      BOOST_CONTAINER_DOCIGN(BOOST_MOVE_I typename dtl::disable_if_or
            < void
            BOOST_MOVE_I dtl::is_convertible<ForwardIterator BOOST_MOVE_I size_type>
            BOOST_MOVE_I dtl::is_input_iterator<ForwardIterator>
            >::type * = 0)
      )
   {
      return insert_range(position, first, last);
   }

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   #if !defined(BOOST_NO_CXX11_HDR_INITIALIZER_LIST)
   /** **Equivalent to**: `insert(position, il.begin(), il.end())` */
   inline iterator insert(const_iterator position, std::initializer_list<T> il)
   {
      return this->insert(position, il.begin(), il.end());
   }
   #endif

   /**
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   *
   * **Effects**: Destroys the element pointed by `position` and removes it from the devector.
   * Invalidates iterators.
   *
   * **Requires**: `T` shall be [MoveAssignable].
   *
   * **Precondition**: `position` must be in the range of `[begin(), end())`.
   *
   * **Returns**: Iterator pointing to the element immediately following the erased element
   * prior to its erasure. If no such element exists, `end()` is returned.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowAssignable`,
   * Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in half the size of `*this`.
   */
   iterator erase(const_iterator position)
   {
      return erase(position, position + 1);
   }

   /**
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   *
   * **Effects**: Destroys the range `[first,last)` and removes it from the devector.
   * Invalidates iterators.
   *
   * **Requires**: `T` shall be [MoveAssignable].
   *
   * **Precondition**: `[first,last)` must be in the range of `[begin(), end())`.
   *
   * **Returns**: Iterator pointing to the element pointed to by `last` prior to any elements
   * being erased. If no such element exists, `end()` is returned.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowAssignable`,
   * Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in half the size of `*this`
   * plus the distance between `first` and `last`.
   */
   iterator erase(const_iterator first, const_iterator last)
   {
      iterator nc_first = unconst_iterator(first);
      iterator nc_last  = unconst_iterator(last);
      return erase(nc_first, nc_last);
   }

   /**
   * [MoveAssignable]: http://en.cppreference.com/w/cpp/concept/MoveAssignable
   *
   * **Effects**: Destroys the range `[first,last)` and removes it from the devector.
   * Invalidates iterators.
   *
   * **Requires**: `T` shall be [MoveAssignable].
   *
   * **Precondition**: `[first,last)` must be in the range of `[begin(), end())`.
   *
   * **Returns**: Iterator pointing to the element pointed to by `last` prior to any elements
   * being erased. If no such element exists, `end()` is returned.
   *
   * **Exceptions**: Strong exception guarantee if `T` is `NothrowAssignable`,
   * Basic exception guarantee otherwise.
   *
   * **Complexity**: Linear in half the size of `*this`.
   */
   iterator erase(iterator first, iterator last)
   {
      size_type front_distance = pos_to_index(last);
      size_type back_distance  = size_type(end() - first);
      size_type n = boost::container::iterator_udistance(first, last);

      if (front_distance < back_distance)
      {
            // move n to the right
            boost::container::move_backward(begin(), first, last);

            for (iterator i = begin(); i != begin() + n; ++i)
            {
               allocator_traits_type::destroy(get_allocator_ref(), boost::movelib::to_raw_pointer(i));
            }
            //n is always less than max stored_size_type
            m_.set_front_idx(m_.front_idx + n);

            BOOST_ASSERT(invariants_ok());
            return last;
      }
      else {
            // move n to the left
            boost::container::move(last, end(), first);

            for (iterator i = end() - n; i != end(); ++i)
            {
               allocator_traits_type::destroy(get_allocator_ref(), boost::movelib::to_raw_pointer(i));
            }
            //n is always less than max stored_size_type
            m_.set_back_idx(m_.back_idx - n);

            BOOST_ASSERT(invariants_ok());
            return first;
      }
   }

   /**
   * [MoveInsertable]: http://en.cppreference.com/w/cpp/concept/MoveInsertable
   *
   * **Effects**: exchanges the contents of `*this` and `b`.
   *
   * **Requires**: instances of `T` must be swappable by unqualified call of `swap`
   * and `T` must be [MoveInsertable] into `*this`.
   *
   * **Precondition**: The allocators should allow propagation or should compare equal.
   *
   * **Exceptions**: Basic exceptions guarantee if not `noexcept`.
   *
   * **Complexity**: Constant.
   */
   void swap(devector& b)
      BOOST_NOEXCEPT_IF( allocator_traits_type::propagate_on_container_swap::value
                                 || allocator_traits_type::is_always_equal::value)
   {
      BOOST_CONSTEXPR_OR_CONST bool propagate_alloc = allocator_traits_type::propagate_on_container_swap::value;
      BOOST_ASSERT(propagate_alloc || get_allocator_ref() == b.get_allocator_ref()); // else it's undefined behavior

      swap_big_big(*this, b);

      // swap indices
      boost::adl_move_swap(m_.front_idx, b.m_.front_idx);
      boost::adl_move_swap(m_.back_idx, b.m_.back_idx);

      //And now swap the allocator
      dtl::swap_alloc(this->get_allocator_ref(), b.get_allocator_ref(), dtl::bool_<propagate_alloc>());

      BOOST_ASSERT(   invariants_ok());
      BOOST_ASSERT(b.invariants_ok());
   }

   /**
   * **Effects**: Destroys all elements in the devector.
   * Invalidates all references, pointers and iterators to the
   * elements of the devector.
   *
   * **Postcondition**: `empty() && front_free_capacity() == 0
   * && back_free_capacity() == old capacity`.
   *
   * **Complexity**: Linear in the size of `*this`.
   *
   * **Remarks**: Does not free memory.
   */
   void clear() BOOST_NOEXCEPT
   {
      destroy_elements(begin(), end());
      m_.front_idx = m_.back_idx = 0;
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator==(const devector& x, const devector& y)
   {   return x.size() == y.size() && ::boost::container::algo_equal(x.begin(), x.end(), y.begin());   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator!=(const devector& x, const devector& y)
   {   return !(x == y); }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator< (const devector& x, const devector& y)
   {   return boost::container::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>(const devector& x, const devector& y)
   {   return y < x;   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator<=(const devector& x, const devector& y)
   {   return !(y < x);   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
      friend bool operator>=(const devector& x, const devector& y)
   {   return !(x < y);   }

   inline friend void swap(devector& x, devector& y)
      BOOST_NOEXCEPT_IF( allocator_traits_type::propagate_on_container_swap::value
                                 || allocator_traits_type::is_always_equal::value)
   {   x.swap(y);   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   //Functions for optimizations, not for users
   T *unused_storage(size_type &sz)
   {
      T *const storage_addr = boost::movelib::to_raw_pointer(m_.buffer);
      if(this->empty()){
         sz = m_.capacity;
         return storage_addr;
      }
      else if(this->back_free_capacity() > this->front_free_capacity()){
         sz = this->back_free_capacity();
         return storage_addr + m_.back_idx;
      }
      else{
         sz = this->front_free_capacity();
         return storage_addr;
      }
   }

   #endif

   private:

   void priv_move_assign(BOOST_RV_REF(devector) x, dtl::bool_<true> /*steal_resources*/)
   {
      this->clear();
      this->deallocate_buffer();

      //Move allocator if needed
      dtl::bool_<allocator_traits_type::
         propagate_on_container_move_assignment::value> flag;
      dtl::move_alloc(this->get_allocator_ref(), x.get_allocator_ref(), flag);

      m_.capacity = x.m_.capacity;
      m_.buffer = x.m_.buffer;
      m_.front_idx = x.m_.front_idx;
      m_.back_idx = x.m_.back_idx;

      // leave x in valid state
      x.m_.capacity = 0u;
      x.m_.buffer = pointer();
      x.m_.back_idx = x.m_.front_idx = 0;
   }

   void priv_move_assign(BOOST_RV_REF(devector) x, dtl::bool_<false> /*steal_resources*/)
   {
      //We can't guarantee a compile-time equal allocator or propagation so fallback to runtime
      //Resources can be transferred if both allocators are equal
      if (get_allocator_ref() == x.get_allocator_ref()) {
         this->priv_move_assign(boost::move(x), dtl::true_());
      }
      else {
         // We can't steal memory.
         move_iterator<iterator> xbegin = boost::make_move_iterator(x.begin());
         move_iterator<iterator> xend = boost::make_move_iterator(x.end());

         //Move allocator if needed
         dtl::bool_<allocator_traits_type::
            propagate_on_container_move_assignment::value> flag;
         dtl::move_alloc(this->get_allocator_ref(), x.get_allocator_ref(), flag);

         if (m_.capacity >= x.size()) {
            overwrite_buffer(xbegin, xend);
         }
         else {
            allocate_and_copy_range(xbegin, xend);
         }
      }
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   size_type pos_to_index(const_iterator i) const 
   {
      return static_cast<size_type>(i - cbegin());
   }

   BOOST_CONTAINER_ATTRIBUTE_NODISCARD inline
   bool should_move_back(const_iterator i) const 
   {
      return static_cast<size_type>(this->pos_to_index(i)) >= this->size()/2u;
   }

   inline static iterator unconst_iterator(const_iterator i)
   {
      return boost::intrusive::pointer_traits<pointer>::const_cast_from(i);
   }

   inline size_type front_capacity() const
   {
      return m_.back_idx;
   }

   inline size_type back_capacity() const
   {
      return size_type(m_.capacity - m_.front_idx);
   }

   #ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

   inline T* priv_raw_begin() BOOST_NOEXCEPT
   {   return boost::movelib::to_raw_pointer(m_.buffer) + m_.front_idx;   }

   inline T* priv_raw_end() BOOST_NOEXCEPT
   {   return boost::movelib::to_raw_pointer(m_.buffer) + m_.back_idx;    }


   template <class U>
   inline void priv_push_front(BOOST_FWD_REF(U) u)
   {
      this->emplace_front(boost::forward<U>(u));
   }

   template <class U>
   inline void priv_push_back(BOOST_FWD_REF(U) u)
   {
      this->emplace_back(boost::forward<U>(u));
   }

   template <class U>
   inline iterator priv_insert(const_iterator pos, BOOST_FWD_REF(U) u)
   {
      return this->emplace(pos, boost::forward<U>(u));
   }

   // allocator_type wrappers

   inline allocator_type& get_allocator_ref() BOOST_NOEXCEPT
   {
      return static_cast<allocator_type&>(m_);
   }

   inline const allocator_type& get_allocator_ref() const BOOST_NOEXCEPT
   {
      return static_cast<const allocator_type&>(m_);
   }

   pointer allocate(size_type cap)
   {
      pointer const p = impl::do_allocate(get_allocator_ref(), cap);
      #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
      ++m_.capacity_alloc_count;
      #endif // BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
      return p;
   }

   void destroy_elements(pointer b, pointer e)
   {
      for (; b != e; ++b)
      {
         allocator_traits_type::destroy(get_allocator_ref(), boost::movelib::to_raw_pointer(b));
      }
   }

   void deallocate_buffer()
   {
      if (m_.buffer)
      {
         allocator_traits_type::deallocate(get_allocator_ref(), m_.buffer, m_.capacity);
      }
   }

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)
   template <typename... Args>
   inline void alloc_construct(pointer dst, Args&&... args)
   {
      allocator_traits_type::construct(
            get_allocator_ref(),
            boost::movelib::to_raw_pointer(dst),
            boost::forward<Args>(args)...
      );
   }

   template <typename... Args>
   void construct_n(pointer buffer, size_type n, Args&&... args)
   {
      detail::construction_guard<allocator_type> ctr_guard(buffer, get_allocator_ref());
      guarded_construct_n(buffer, n, ctr_guard, boost::forward<Args>(args)...);
      ctr_guard.release();
   }

   template <typename... Args>
   void guarded_construct_n(pointer buffer, size_type n, detail::construction_guard<allocator_type>& ctr_guard, Args&&... args)
   {
      for (size_type i = 0; i < n; ++i) {
         this->alloc_construct(buffer + i, boost::forward<Args>(args)...);
         ctr_guard.extend();
      }
   }

   #else //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_CONTAINER_DEVECTOR_ALLOC_CONSTRUCT(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   inline void alloc_construct(pointer dst BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      allocator_traits_type::construct(\
            get_allocator_ref(), boost::movelib::to_raw_pointer(dst) BOOST_MOVE_I##N BOOST_MOVE_FWD##N );\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void construct_n(pointer buffer, size_type n BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      detail::construction_guard<allocator_type> ctr_guard(buffer, get_allocator_ref());\
      guarded_construct_n(buffer, n, ctr_guard BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
      ctr_guard.release();\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void guarded_construct_n(pointer buffer, size_type n, detail::construction_guard<allocator_type>& ctr_guard BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      for (size_type i = 0; i < n; ++i) {\
            this->alloc_construct(buffer + i BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
            ctr_guard.extend();\
      }\
   }
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_DEVECTOR_ALLOC_CONSTRUCT)
   #undef BOOST_CONTAINER_DEVECTOR_ALLOC_CONSTRUCT

   #endif    //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   size_type calculate_new_capacity(size_type requested_capacity)
   {
      size_type max = allocator_traits_type::max_size(this->get_allocator_ref());
      (clamp_by_stored_size_type)(max, stored_size_type());
      const size_type remaining_additional_cap = max - size_type(m_.capacity);
      const size_type min_additional_cap = requested_capacity - size_type(m_.capacity);
      if ( remaining_additional_cap < min_additional_cap )
            boost::container::throw_length_error("devector: get_next_capacity, max size exceeded");

      return growth_factor_type()( size_type(m_.capacity), min_additional_cap, max);
   }

   void buffer_move_or_copy(pointer dst)
   {
      detail::construction_guard<allocator_type> guard(dst, get_allocator_ref());

      buffer_move_or_copy(dst, guard);

      guard.release();
   }

   void buffer_move_or_copy(pointer dst, detail::construction_guard<allocator_type>& guard)
   {
      opt_move_or_copy(begin(), end(), dst, guard);

      destroy_elements(data(), data() + size());
      deallocate_buffer();
   }

   template <typename Guard>
   void opt_move_or_copy(pointer b, pointer e, pointer dst, Guard& guard)
   {
      // if trivial copy and default allocator, memcpy
      boost::container::uninitialized_move_alloc(get_allocator_ref(), b, e, dst);
      guard.extend();
   }

   #if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   template <typename... Args>
   void resize_impl(size_type sz, Args&&... args)
   {
      const size_type old_sz = this->size();
      if (sz > old_sz)
      {
         const size_type n = sz - old_sz;

         if (sz <= m_.capacity)
         {
            //Construct at back
            const size_type bfc = this->back_free_capacity();
            const size_type b = n < bfc ? n : bfc;
            construct_n(m_.buffer + m_.back_idx, b, boost::forward<Args>(args)...);
            m_.set_back_idx(m_.back_idx + b);

            //Construct remaining at front
            const size_type f = n - b;
            construct_n(m_.buffer + m_.front_idx - f, f, boost::forward<Args>(args)...);
            m_.set_front_idx(m_.front_idx - f);
         }
         else
         {
            resize_back_slow_path(sz, n, boost::forward<Args>(args)...);
         }
      }
      else
      {
         const size_type n = old_sz - sz;
         const size_type new_bidx = m_.back_idx - n;
         destroy_elements(m_.buffer + new_bidx, m_.buffer + m_.back_idx);
         m_.set_back_idx(new_bidx);
      }
   }

   template <typename... Args>
   void resize_front_impl(size_type sz , Args&&... args)
   {
      const size_type old_sz = this->size();
      if (sz > old_sz)
      {
         const size_type n = sz - old_sz;

         if (sz <= this->front_capacity())
         {
            construct_n(m_.buffer + m_.front_idx - n, n, boost::forward<Args>(args)...);
            m_.set_front_idx(m_.front_idx - n);
         }
         else
         {
            resize_front_slow_path(sz, n, boost::forward<Args>(args)...);
         }
      }
      else {
         const size_type n = old_sz - sz;
         const size_type new_fidx = m_.front_idx + n;
         destroy_elements(m_.buffer + m_.front_idx, m_.buffer + new_fidx);
         m_.set_front_idx(new_fidx);
      }
   }

   template <typename... Args>
   void resize_front_slow_path(size_type sz, size_type n, Args&&... args)
   {
      const size_type new_capacity = calculate_new_capacity(sz + back_free_capacity());
      pointer new_buffer = allocate(new_capacity);
      allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());

      const size_type old_sz = this->size();
      const size_type new_old_elem_index = new_capacity - old_sz;
      const size_type new_elem_index = new_old_elem_index - n;

      detail::construction_guard<allocator_type> guard(new_buffer + new_elem_index, get_allocator_ref());
      guarded_construct_n(new_buffer + new_elem_index, n, guard, boost::forward<Args>(args)...);

      buffer_move_or_copy(new_buffer + new_old_elem_index, guard);

      guard.release();
      new_buffer_guard.release();

      m_.buffer = new_buffer;
      m_.set_capacity(new_capacity);
      m_.set_front_idx(new_elem_index);
      m_.set_back_idx(new_elem_index + old_sz + n);
   }

   template <typename... Args>
   void resize_back_impl(size_type sz, Args&&... args)
   {
      const size_type old_sz = this->size();
      if (sz > old_sz)
      {
         const size_type n = sz - old_sz;

         if (sz <= this->back_capacity())
         {
            construct_n(m_.buffer + m_.back_idx, n, boost::forward<Args>(args)...);
            m_.set_back_idx(m_.back_idx + n);
         }
         else
         {
            resize_back_slow_path(sz, n, boost::forward<Args>(args)...);
         }
      }
      else
      {
         const size_type n = old_sz - sz;
         const size_type new_bidx = m_.back_idx - n;
         destroy_elements(m_.buffer + new_bidx, m_.buffer + m_.back_idx);
         m_.set_back_idx(new_bidx);
      }
   }

   template <typename... Args>
   void resize_back_slow_path(size_type sz, size_type n, Args&&... args)
   {
      const size_type new_capacity = calculate_new_capacity(sz + front_free_capacity());
      pointer new_buffer = allocate(new_capacity);
      allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());

      detail::construction_guard<allocator_type> guard(new_buffer + m_.back_idx, get_allocator_ref());
      guarded_construct_n(new_buffer + m_.back_idx, n, guard, boost::forward<Args>(args)...);

      buffer_move_or_copy(new_buffer + m_.front_idx);

      guard.release();
      new_buffer_guard.release();

      m_.buffer = new_buffer;
      m_.set_capacity(new_capacity);
      m_.set_back_idx(m_.back_idx + n);
   }

   #else //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   #define BOOST_CONTAINER_DEVECTOR_SLOW_PATH(N) \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void resize_front_impl(size_type sz BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      if (sz > size())\
      {\
         const size_type n = sz - size();\
         if (sz <= front_capacity()){\
            construct_n(m_.buffer + m_.front_idx - n, n BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
            m_.set_front_idx(m_.front_idx - n);\
         }\
         else\
         {\
            resize_front_slow_path(sz, n BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         }\
      }\
      else {\
         while (this->size() > sz)\
         {\
            this->pop_front();\
         }\
      }\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void resize_front_slow_path(size_type sz, size_type n BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      const size_type new_capacity = calculate_new_capacity(sz + back_free_capacity());\
      pointer new_buffer = allocate(new_capacity);\
      allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());\
   \
      const size_type new_old_elem_index = new_capacity - size();\
      const size_type new_elem_index = new_old_elem_index - n;\
   \
      detail::construction_guard<allocator_type> guard(new_buffer + new_elem_index, get_allocator_ref());\
      guarded_construct_n(new_buffer + new_elem_index, n, guard BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
   \
      buffer_move_or_copy(new_buffer + new_old_elem_index, guard);\
   \
      guard.release();\
      new_buffer_guard.release();\
      m_.buffer = new_buffer;\
      m_.set_capacity(new_capacity);\
      m_.set_back_idx(new_old_elem_index + m_.back_idx - m_.front_idx);\
      m_.set_front_idx(new_elem_index);\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void resize_back_impl(size_type sz BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      if (sz > size())\
      {\
         const size_type n = sz - size();\
      \
         if (sz <= back_capacity())\
         {\
            construct_n(m_.buffer + m_.back_idx, n BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
            m_.set_back_idx(m_.back_idx + n);\
         }\
         else\
         {\
            resize_back_slow_path(sz, n BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
         }\
      }\
      else\
      {\
         while (size() > sz)\
         {\
            pop_back();\
         }\
      }\
   }\
   \
   BOOST_MOVE_TMPL_LT##N BOOST_MOVE_CLASS##N BOOST_MOVE_GT##N \
   void resize_back_slow_path(size_type sz, size_type n BOOST_MOVE_I##N BOOST_MOVE_UREF##N)\
   {\
      const size_type new_capacity = calculate_new_capacity(sz + front_free_capacity());\
      pointer new_buffer = allocate(new_capacity);\
      allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());\
   \
      detail::construction_guard<allocator_type> guard(new_buffer + m_.back_idx, get_allocator_ref());\
      guarded_construct_n(new_buffer + m_.back_idx, n, guard BOOST_MOVE_I##N BOOST_MOVE_FWD##N);\
   \
      buffer_move_or_copy(new_buffer + m_.front_idx);\
   \
      guard.release();\
      new_buffer_guard.release();\
   \
      m_.buffer = new_buffer;\
      m_.set_capacity(new_capacity);\
      m_.set_back_idx(m_.back_idx + n);\
   }\
   \
   //
   BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_DEVECTOR_SLOW_PATH)
   #undef BOOST_CONTAINER_DEVECTOR_SLOW_PATH

   #endif    //!defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) || defined(BOOST_CONTAINER_DOXYGEN_INVOKED)

   void reallocate_at(size_type new_capacity, size_type buffer_offset)
   {
      pointer new_buffer = allocate(new_capacity);
      {
         allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());
         boost::container::uninitialized_move_alloc(get_allocator_ref(), this->begin(), this->end(), new_buffer + buffer_offset);
         new_buffer_guard.release();
      }
      destroy_elements(m_.buffer + m_.front_idx, m_.buffer + m_.back_idx);
      deallocate_buffer();

      m_.buffer = new_buffer;
      //Safe cast, allocate() will handle stored_size_type overflow
      m_.set_capacity(new_capacity);
      m_.set_back_idx(size_type(m_.back_idx - m_.front_idx) + buffer_offset);
      m_.set_front_idx(buffer_offset);

      BOOST_ASSERT(invariants_ok());
   }

   template <typename ForwardIterator>
   iterator insert_range(const_iterator position, ForwardIterator first, ForwardIterator last)
   {
      BOOST_ASSERT(position >= begin());
      BOOST_ASSERT(position <= end());
      typedef dtl::insert_range_proxy<allocator_type, ForwardIterator> proxy_t;

      size_type const n = boost::container::iterator_udistance(first, last);
      bool prefer_move_back;
      if (BOOST_UNLIKELY(!n)) {
         return begin() + size_type(position - cbegin());
      }
      else if (position == end()) {
         if(back_free_capacity() >= n) // fast path
         {
            iterator r(this->end());
            boost::container::uninitialized_copy_alloc(get_allocator_ref(), first, last, this->priv_raw_end());
            m_.set_back_idx(m_.back_idx + n);
            return r;
         }
         prefer_move_back = true;
      }
      else if (position == begin()) {
         if(front_free_capacity() >= n) {// secondary fast path
            boost::container::uninitialized_copy_alloc(get_allocator_ref(), first, last, this->priv_raw_begin() - n);
            m_.set_front_idx(m_.front_idx - n);
            return begin();
         }
         prefer_move_back = false;
      }
      else{
         iterator nonconst_pos = unconst_iterator(position);
         prefer_move_back = should_move_back(position);

         if(prefer_move_back){
            if(back_free_capacity() >= n){
               boost::container::expand_forward_and_insert_nonempty_middle_alloc
                  ( get_allocator_ref()
                  , boost::movelib::to_raw_pointer(nonconst_pos)
                  , this->priv_raw_end()
                  , n, proxy_t(first));
               m_.set_back_idx(m_.back_idx + n);
               return nonconst_pos;
            }
         }
         else{
            if (front_free_capacity() >= n){
               boost::container::expand_backward_and_insert_nonempty_middle_alloc
                  ( get_allocator_ref()
                  , this->priv_raw_begin()
                  , boost::movelib::to_raw_pointer(nonconst_pos)
                  , n, proxy_t(first));
               m_.set_front_idx(m_.front_idx - n);
               return (nonconst_pos -= n);
            }
         }
      }
      return this->insert_range_slow_path(position, n, proxy_t(first));
   }

   template <class InsertionProxy>
   BOOST_CONTAINER_NOINLINE iterator insert_range_slow_path
      (const_iterator p, const size_type n, const InsertionProxy proxy)
   {
      size_type const back_free_cap = back_free_capacity();
      size_type const front_free_cap = front_free_capacity();
      size_type const free_cap = front_free_cap + back_free_cap;
      size_type const index = size_type(p - cbegin());

      size_type const cap = m_.capacity;
      //Test if enough free memory would be left
      if (free_cap >= n && (free_cap - n) >= cap/devector_min_free_fraction) {
         size_type const old_size = this->size();
         T* const raw_pos = const_cast<T*>(boost::movelib::to_raw_pointer(p));
         size_type const new_size = old_size + n;
         size_type const new_front_idx = (cap - new_size) / 2u;

         T* const raw_beg = this->priv_raw_begin();
         T* const new_raw_beg = raw_beg - std::ptrdiff_t(m_.front_idx - new_front_idx);
         m_.back_idx = 0u;
         m_.front_idx = 0u;
         boost::container::expand_backward_forward_and_insert_alloc
            (raw_beg, old_size, new_raw_beg, raw_pos, n, proxy, get_allocator_ref());
         m_.set_front_idx(new_front_idx);
         m_.set_back_idx(new_front_idx + new_size);
      }
      else {
         // reallocate
         const size_type new_capacity = calculate_new_capacity(m_.capacity + n);
         pointer new_buffer = allocate(new_capacity);

         // guard allocation
         allocation_guard new_buffer_guard(new_buffer, new_capacity, get_allocator_ref());

         size_type const old_size = this->size();
         const size_type new_front_index = (new_capacity - old_size - n) / 2u;

         T* const raw_pos = const_cast<T*>(boost::movelib::to_raw_pointer(p));
         T* const raw_new_start = const_cast<T*>(boost::movelib::to_raw_pointer(new_buffer)) + new_front_index;

         boost::container::uninitialized_move_and_insert_alloc
            (get_allocator_ref(), this->priv_raw_begin(), raw_pos, this->priv_raw_end(), raw_new_start, n, proxy);
         new_buffer_guard.release();

         // cleanup
         destroy_elements(begin(), end());
         deallocate_buffer();

         // rebind members
         m_.set_capacity(new_capacity);
         m_.buffer = new_buffer;
         m_.set_back_idx(new_front_index + old_size + n);
         m_.set_front_idx(new_front_index);
      }
      return begin() + index;
   }


   template <typename Iterator>
   void construct_from_range(Iterator b, Iterator e)
   {
      allocation_guard buffer_guard(m_.buffer, m_.capacity, get_allocator_ref());
      boost::container::uninitialized_copy_alloc(get_allocator_ref(), b, e, m_.buffer);
      buffer_guard.release();
   }

   template <typename ForwardIterator>
   void allocate_and_copy_range(ForwardIterator first, ForwardIterator last)
   {
      size_type n = boost::container::iterator_udistance(first, last);

      pointer new_buffer = n ? allocate(n) : pointer();
      allocation_guard new_buffer_guard(new_buffer, n, get_allocator_ref());
      boost::container::uninitialized_copy_alloc(get_allocator_ref(), first, last, new_buffer);
      destroy_elements(begin(), end());
      deallocate_buffer();

      m_.set_capacity(n);
      m_.buffer = new_buffer;
      m_.front_idx = 0;
      m_.set_back_idx(n);

      new_buffer_guard.release();
   }

   static void swap_big_big(devector& a, devector& b) BOOST_NOEXCEPT
   {
      boost::adl_move_swap(a.m_.capacity, b.m_.capacity);
      boost::adl_move_swap(a.m_.buffer, b.m_.buffer);
   }

   template <typename ForwardIterator>
   void overwrite_buffer_impl(ForwardIterator first, ForwardIterator last, dtl::true_)
   {
      const size_type n = boost::container::iterator_udistance(first, last);

      BOOST_ASSERT(m_.capacity >= n);
      boost::container::uninitialized_copy_alloc_n
            ( get_allocator_ref(), first
            , n, boost::movelib::to_raw_pointer(m_.buffer));
      m_.front_idx = 0;
      m_.set_back_idx(n);
   }

   template <typename InputIterator>
   InputIterator overwrite_buffer_impl(InputIterator first, InputIterator last, dtl::false_)
   {
      pointer pos = m_.buffer;
      detail::construction_guard<allocator_type> front_guard(pos, get_allocator_ref());

      while (first != last && pos != begin()) {
         this->alloc_construct(pos++, *first++);
         front_guard.extend();
      }

      while (first != last && pos != end()) {
         *pos++ = *first++;
      }

      detail::construction_guard<allocator_type> back_guard(pos, get_allocator_ref());

      iterator capacity_end = m_.buffer + m_.capacity;
      while (first != last && pos != capacity_end) {
         this->alloc_construct(pos++, *first++);
         back_guard.extend();
      }

      pointer destroy_after = dtl::min_value(dtl::max_value(begin(), pos), end());
      destroy_elements(destroy_after, end());

      front_guard.release();
      back_guard.release();

      m_.front_idx = 0;
      m_.set_back_idx(pos_to_index(pos));
      return first;
   }

   template <typename ForwardIterator>
   inline void overwrite_buffer(ForwardIterator first, ForwardIterator last)
   {
      this->overwrite_buffer_impl(first, last, 
            dtl::bool_<dtl::is_trivially_destructible<T>::value>());
   }

   bool invariants_ok()
   {
      return  (! m_.capacity || m_.buffer )
              && m_.front_idx <= m_.back_idx
              && m_.back_idx <= m_.capacity;
   }

   struct impl : allocator_type
   {
      BOOST_MOVABLE_BUT_NOT_COPYABLE(impl)

      public:
      allocator_type &get_al()
      {   return *this;   }

      static pointer do_allocate(allocator_type &a, size_type cap)
      {
         if (cap) {
            //First detect overflow on smaller stored_size_types
            if (cap > stored_size_type(-1)){
                  boost::container::throw_length_error("get_next_capacity, allocator's max size reached");
            }
            return allocator_traits_type::allocate(a, cap);
         }
         else {
            return pointer();
         }
      }

      impl()
         : allocator_type(), buffer(), front_idx(), back_idx(), capacity()
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(0)
         #endif
      {}

      explicit impl(const allocator_type &a)
         : allocator_type(a), buffer(), front_idx(), back_idx(), capacity()
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(0)
         #endif
      {}

      impl(reserve_uninitialized_t, const allocator_type& a, size_type c)
         : allocator_type(a), buffer(do_allocate(get_al(), c) )
         //static cast sizes, as the allocation function will take care of overflows
         , front_idx(static_cast<stored_size_type>(0u))
         , back_idx(static_cast<stored_size_type>(c))
         , capacity(static_cast<stored_size_type>(c))
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(size_type(buffer != pointer()))
         #endif
      {}

      impl(reserve_only_tag_t, const allocator_type &a, size_type const ffc, size_type const bfc)
         : allocator_type(a), buffer(do_allocate(get_al(), ffc+bfc) )
         //static cast sizes, as the allocation function will take care of overflows
         , front_idx(static_cast<stored_size_type>(ffc))
         , back_idx(static_cast<stored_size_type>(ffc))
         , capacity(static_cast<stored_size_type>(ffc + bfc))
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(size_type(buffer != pointer()))
         #endif
      {}

      impl(reserve_only_tag_t, const allocator_type &a, size_type const c)
         : allocator_type(a), buffer(do_allocate(get_al(), c) )
         //static cast sizes, as the allocation function will take care of overflows
         , front_idx(static_cast<stored_size_type>(c/2u))
         , back_idx(static_cast<stored_size_type>(c/2u))
         , capacity(static_cast<stored_size_type>(c))
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(size_type(buffer != pointer()))
         #endif
      {}

      impl(review_implementation_t, const allocator_type &a, pointer p, size_type fi, size_type bi, size_type c)
         : allocator_type(a), buffer(p)
         //static cast sizes, as the allocation function will take care of overflows
         , front_idx(static_cast<stored_size_type>(fi))
         , back_idx(static_cast<stored_size_type>(bi))
         , capacity(static_cast<stored_size_type>(c))
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(0)
         #endif
      {}

      impl(BOOST_RV_REF(impl) m)
         : allocator_type(BOOST_MOVE_BASE(allocator_type, m))
         , buffer(static_cast<impl&>(m).buffer)
         , front_idx(static_cast<impl&>(m).front_idx)
         , back_idx(static_cast<impl&>(m).back_idx)
         , capacity(static_cast<impl&>(m).capacity)
         #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
         , capacity_alloc_count(0)
         #endif
      {
         impl &i = static_cast<impl&>(m);
         // buffer is already acquired, reset rhs
         i.capacity = 0u;
         i.buffer = pointer();
         i.front_idx = 0;
         i.back_idx = 0;
      }

      inline void set_back_idx(size_type bi)
      {
         back_idx = static_cast<stored_size_type>(bi);
      }

      inline void set_front_idx(size_type fi)
      {
         front_idx = static_cast<stored_size_type>(fi);
      }

      inline void set_capacity(size_type c)
      {
         capacity = static_cast<stored_size_type>(c);
      }

      pointer           buffer;
      stored_size_type  front_idx;
      stored_size_type  back_idx;
      stored_size_type  capacity;
      #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
      size_type capacity_alloc_count;
      #endif
   } m_;

   #ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS
   public:
   void reset_alloc_stats()
   {
      m_.capacity_alloc_count = 0;
   }

   size_type get_alloc_count() const
   {
      return m_.capacity_alloc_count;
   }

   #endif // ifdef BOOST_CONTAINER_DEVECTOR_ALLOC_STATS

   #endif // ifndef BOOST_CONTAINER_DOXYGEN_INVOKED
};

}} // namespace boost::container

#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

namespace boost {

//!has_trivial_destructor_after_move<> == true_type
//!specialization for optimizations
template <class T, class Allocator, class Options>
struct has_trivial_destructor_after_move<boost::container::devector<T, Allocator, Options> >
{
    typedef typename boost::container::devector<T, Allocator, Options>::allocator_type allocator_type;
    typedef typename ::boost::container::allocator_traits<allocator_type>::pointer pointer;
    BOOST_STATIC_CONSTEXPR bool value =
      ::boost::has_trivial_destructor_after_move<allocator_type>::value &&
      ::boost::has_trivial_destructor_after_move<pointer>::value;
};

}

#endif    //#ifndef BOOST_CONTAINER_DOXYGEN_INVOKED

#include <boost/container/detail/config_end.hpp>

#endif // BOOST_CONTAINER_DEVECTOR_HPP
