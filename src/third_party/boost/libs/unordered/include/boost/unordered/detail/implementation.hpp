// Copyright (C) 2003-2004 Jeremy B. Maitin-Shepard.
// Copyright (C) 2005-2016 Daniel James
// Copyright (C) 2022-2024 Joaquin M Lopez Munoz.
// Copyright (C) 2022-2023 Christian Mazakas
// Copyright (C) 2024 Braden Ganetsky
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNORDERED_DETAIL_IMPLEMENTATION_HPP
#define BOOST_UNORDERED_DETAIL_IMPLEMENTATION_HPP

#include <boost/config.hpp>
#if defined(BOOST_HAS_PRAGMA_ONCE)
#pragma once
#endif

#include <boost/unordered/detail/allocator_constructed.hpp>
#include <boost/unordered/detail/fca.hpp>
#include <boost/unordered/detail/opt_storage.hpp>
#include <boost/unordered/detail/serialize_tracked_address.hpp>
#include <boost/unordered/detail/static_assert.hpp>
#include <boost/unordered/detail/type_traits.hpp>
#include <boost/unordered/unordered_printers.hpp>

#include <boost/assert.hpp>
#include <boost/core/allocator_traits.hpp>
#include <boost/core/bit.hpp>
#include <boost/core/invoke_swap.hpp>
#include <boost/core/no_exceptions_support.hpp>
#include <boost/core/pointer_traits.hpp>
#include <boost/core/serialization.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <tuple> // std::forward_as_tuple

namespace boost {
  namespace tuples {
    struct null_type;
  }
} // namespace boost

// BOOST_UNORDERED_SUPPRESS_DEPRECATED
//
// Define to stop deprecation attributes

#if defined(BOOST_UNORDERED_SUPPRESS_DEPRECATED)
#define BOOST_UNORDERED_DEPRECATED(msg)
#endif

// BOOST_UNORDERED_DEPRECATED
//
// Wrapper around various depreaction attributes.

#if defined(__has_cpp_attribute) &&                                            \
  (!defined(__cplusplus) || __cplusplus >= 201402)
#if __has_cpp_attribute(deprecated) && !defined(BOOST_UNORDERED_DEPRECATED)
#define BOOST_UNORDERED_DEPRECATED(msg) [[deprecated(msg)]]
#endif
#endif

#if !defined(BOOST_UNORDERED_DEPRECATED)
#if defined(__GNUC__) && __GNUC__ >= 4
#define BOOST_UNORDERED_DEPRECATED(msg) __attribute__((deprecated))
#elif defined(_MSC_VER) && _MSC_VER >= 1400
#define BOOST_UNORDERED_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(_MSC_VER) && _MSC_VER >= 1310
#define BOOST_UNORDERED_DEPRECATED(msg) __declspec(deprecated)
#else
#define BOOST_UNORDERED_DEPRECATED(msg)
#endif
#endif

namespace boost {
  namespace unordered {

    using std::piecewise_construct;
    using std::piecewise_construct_t;

    namespace detail {

      template <typename Types> struct table;

      static const float minimum_max_load_factor = 1e-3f;
      static const std::size_t default_bucket_count = 0;

      struct move_tag
      {
      };

      struct empty_emplace
      {
      };

      struct no_key
      {
        no_key() {}
        template <class T> no_key(T const&) {}
      };

      struct converting_key
      {
      };

      namespace func {
        template <class T> inline void ignore_unused_variable_warning(T const&)
        {
        }
      } // namespace func

      //////////////////////////////////////////////////////////////////////////
      // iterator SFINAE

      template <typename I>
      struct is_forward : std::is_base_of<std::forward_iterator_tag,
                            typename std::iterator_traits<I>::iterator_category>
      {
      };

      template <typename I, typename ReturnType>
      struct enable_if_forward
          : std::enable_if<boost::unordered::detail::is_forward<I>::value,
              ReturnType>
      {
      };

      template <typename I, typename ReturnType>
      struct disable_if_forward
          : std::enable_if<!boost::unordered::detail::is_forward<I>::value,
              ReturnType>
      {
      };
    } // namespace detail
  } // namespace unordered
} // namespace boost

namespace boost {
  namespace unordered {
    namespace detail {
      //////////////////////////////////////////////////////////////////////////
      // insert_size/initial_size

      template <class I>
      inline typename boost::unordered::detail::enable_if_forward<I,
        std::size_t>::type
      insert_size(I i, I j)
      {
        return static_cast<std::size_t>(std::distance(i, j));
      }

      template <class I>
      inline typename boost::unordered::detail::disable_if_forward<I,
        std::size_t>::type
      insert_size(I, I)
      {
        return 1;
      }

      template <class I>
      inline std::size_t initial_size(I i, I j,
        std::size_t num_buckets =
          boost::unordered::detail::default_bucket_count)
      {
        return (std::max)(
          boost::unordered::detail::insert_size(i, j), num_buckets);
      }

      //////////////////////////////////////////////////////////////////////////
      // compressed

      template <typename T, int Index>
      struct compressed_base : boost::empty_value<T>
      {
        compressed_base(T const& x) : empty_value<T>(boost::empty_init_t(), x)
        {
        }
        compressed_base(T& x, move_tag)
            : empty_value<T>(boost::empty_init_t(), std::move(x))
        {
        }

        T& get() { return empty_value<T>::get(); }
        T const& get() const { return empty_value<T>::get(); }
      };

      template <typename T, int Index>
      struct generate_base : boost::unordered::detail::compressed_base<T, Index>
      {
        typedef compressed_base<T, Index> type;

        generate_base() : type() {}
      };

      template <typename T1, typename T2>
      struct compressed
          : private boost::unordered::detail::generate_base<T1, 1>::type,
            private boost::unordered::detail::generate_base<T2, 2>::type
      {
        typedef typename generate_base<T1, 1>::type base1;
        typedef typename generate_base<T2, 2>::type base2;

        typedef T1 first_type;
        typedef T2 second_type;

        first_type& first() { return static_cast<base1*>(this)->get(); }

        first_type const& first() const
        {
          return static_cast<base1 const*>(this)->get();
        }

        second_type& second() { return static_cast<base2*>(this)->get(); }

        second_type const& second() const
        {
          return static_cast<base2 const*>(this)->get();
        }

        template <typename First, typename Second>
        compressed(First const& x1, Second const& x2) : base1(x1), base2(x2)
        {
        }

        compressed(compressed const& x) : base1(x.first()), base2(x.second()) {}

        compressed(compressed& x, move_tag m)
            : base1(x.first(), m), base2(x.second(), m)
        {
        }

        void assign(compressed const& x)
        {
          first() = x.first();
          second() = x.second();
        }

        void move_assign(compressed& x)
        {
          first() = std::move(x.first());
          second() = std::move(x.second());
        }

        void swap(compressed& x)
        {
          boost::core::invoke_swap(first(), x.first());
          boost::core::invoke_swap(second(), x.second());
        }

      private:
        // Prevent assignment just to make use of assign or
        // move_assign explicit.
        compressed& operator=(compressed const&);
      };

      //////////////////////////////////////////////////////////////////////////
      // pair_traits
      //
      // Used to get the types from a pair without instantiating it.

      template <typename Pair> struct pair_traits
      {
        typedef typename Pair::first_type first_type;
        typedef typename Pair::second_type second_type;
      };

      template <typename T1, typename T2> struct pair_traits<std::pair<T1, T2> >
      {
        typedef T1 first_type;
        typedef T2 second_type;
      };

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable : 4512) // assignment operator could not be generated.
#pragma warning(disable : 4345) // behavior change: an object of POD type
// constructed with an initializer of the form ()
// will be default-initialized.
#endif

      //////////////////////////////////////////////////////////////////////////
      // Bits and pieces for implementing traits

      template <typename T> typename std::add_lvalue_reference<T>::type make();
      struct choice2
      {
        typedef char (&type)[2];
      };
      struct choice1 : choice2
      {
        typedef char (&type)[1];
      };
      choice1 choose();

      typedef choice1::type yes_type;
      typedef choice2::type no_type;

      struct private_type
      {
        private_type const& operator,(int) const;
      };

      template <typename T> no_type is_private_type(T const&);
      yes_type is_private_type(private_type const&);

      struct convert_from_anything
      {
        template <typename T> convert_from_anything(T const&);
      };
    } // namespace detail
  } // namespace unordered
} // namespace boost

////////////////////////////////////////////////////////////////////////////////
//
// Some utilities for implementing allocator_traits, but useful elsewhere so
// they're always defined.

namespace boost {
  namespace unordered {
    namespace detail {

      ////////////////////////////////////////////////////////////////////////////
      // Explicitly call a destructor

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#endif

      namespace func {
        template <class T> inline void destroy(T* x) { x->~T(); }
      } // namespace func

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

      //////////////////////////////////////////////////////////////////////////
      // value_base
      //
      // Space used to store values.

      template <typename ValueType> struct value_base
      {
        typedef ValueType value_type;

        opt_storage<value_type> data_;

        value_base() : data_() {}

        void* address() { return this; }

        value_type& value() { return *(ValueType*)this; }

        value_type const& value() const { return *(ValueType const*)this; }

        value_type* value_ptr() { return (ValueType*)this; }

        value_type const* value_ptr() const { return (ValueType const*)this; }

      private:
        value_base& operator=(value_base const&);
      };

      //////////////////////////////////////////////////////////////////////////
      // optional
      // TODO: Use std::optional when available.

      template <typename T> class optional
      {
        boost::unordered::detail::value_base<T> value_;
        bool has_value_;

        void destroy()
        {
          if (has_value_) {
            boost::unordered::detail::func::destroy(value_.value_ptr());
            has_value_ = false;
          }
        }

        void move(optional<T>& x)
        {
          BOOST_ASSERT(!has_value_ && x.has_value_);
          new (value_.value_ptr()) T(std::move(x.value_.value()));
          boost::unordered::detail::func::destroy(x.value_.value_ptr());
          has_value_ = true;
          x.has_value_ = false;
        }

      public:
        optional() noexcept : has_value_(false) {}

        optional(optional const&) = delete;
        optional& operator=(optional const&) = delete;

        optional(optional<T>&& x) : has_value_(false)
        {
          if (x.has_value_) {
            move(x);
          }
        }

        explicit optional(T const& x) : has_value_(true)
        {
          new (value_.value_ptr()) T(x);
        }

        optional& operator=(optional<T>&& x)
        {
          destroy();
          if (x.has_value_) {
            move(x);
          }
          return *this;
        }

        ~optional() { destroy(); }

        bool has_value() const { return has_value_; }
        T& operator*() { return value_.value(); }
        T const& operator*() const { return value_.value(); }
        T* operator->() { return value_.value_ptr(); }
        T const* operator->() const { return value_.value_ptr(); }

        bool operator==(optional<T> const& x) const
        {
          return has_value_ ? x.has_value_ && value_.value() == x.value_.value()
                            : !x.has_value_;
        }

        bool operator!=(optional<T> const& x) const { return !((*this) == x); }

        void swap(optional<T>& x)
        {
          if (has_value_ != x.has_value_) {
            if (has_value_) {
              x.move(*this);
            } else {
              move(x);
            }
          } else if (has_value_) {
            boost::core::invoke_swap(value_.value(), x.value_.value());
          }
        }

        friend void swap(optional<T>& x, optional<T>& y) { x.swap(y); }
      };
    } // namespace detail
  } // namespace unordered
} // namespace boost

////////////////////////////////////////////////////////////////////////////////
//
// Allocator traits
//

namespace boost {
  namespace unordered {
    namespace detail {

      template <typename Alloc>
      struct allocator_traits : boost::allocator_traits<Alloc>
      {
      };

      template <typename Alloc, typename T>
      struct rebind_wrap : boost::allocator_rebind<Alloc, T>
      {
      };
    } // namespace detail
  } // namespace unordered
} // namespace boost

namespace boost {
  namespace unordered {
    namespace detail {
      namespace func {
        ////////////////////////////////////////////////////////////////////////
        // Trait to check for piecewise construction.

        template <typename A0> struct use_piecewise
        {
          static choice1::type test(choice1, std::piecewise_construct_t);

          static choice2::type test(choice2, ...);

          enum
          {
            value = sizeof(choice1::type) ==
                    sizeof(test(choose(), boost::unordered::detail::make<A0>()))
          };
        };

        ////////////////////////////////////////////////////////////////////////
        // Construct from variadic parameters

        template <typename Alloc, typename T, typename... Args>
        inline void construct_from_args(
          Alloc& alloc, T* address, Args&&... args)
        {
          boost::allocator_construct(
            alloc, address, std::forward<Args>(args)...);
        }

        // For backwards compatibility, implement a special case for
        // piecewise_construct with boost::tuple

        template <typename A0> struct detect_std_tuple
        {
          template <class... Args>
          static choice1::type test(choice1, std::tuple<Args...> const&);

          static choice2::type test(choice2, ...);

          enum
          {
            value = sizeof(choice1::type) ==
                    sizeof(test(choose(), boost::unordered::detail::make<A0>()))
          };
        };

        // Special case for piecewise_construct

        template <template <class...> class Tuple, class... Args,
          std::size_t... Is, class... TupleArgs>
        std::tuple<typename std::add_lvalue_reference<Args>::type...>
        to_std_tuple_impl(boost::mp11::mp_list<Args...>,
          Tuple<TupleArgs...>& tuple, boost::mp11::index_sequence<Is...>)
        {
          (void)tuple;
          using std::get;
          return std::tuple<typename std::add_lvalue_reference<Args>::type...>(
            get<Is>(tuple)...);
        }

        template <class T>
        using add_lvalue_reference_t =
          typename std::add_lvalue_reference<T>::type;

        template <template <class...> class Tuple, class... Args>
        boost::mp11::mp_transform<add_lvalue_reference_t,
          boost::mp11::mp_remove<std::tuple<Args...>,
            boost::tuples::null_type> >
        to_std_tuple(Tuple<Args...>& tuple)
        {
          using list = boost::mp11::mp_remove<boost::mp11::mp_list<Args...>,
            boost::tuples::null_type>;
          using list_size = boost::mp11::mp_size<list>;
          using index_seq = boost::mp11::make_index_sequence<list_size::value>;

          return to_std_tuple_impl(list{}, tuple, index_seq{});
        }

        template <typename Alloc, typename A, typename B, typename A0,
          typename A1, typename A2>
        inline typename std::enable_if<use_piecewise<A0>::value &&
                                         !detect_std_tuple<A1>::value &&
                                         !detect_std_tuple<A2>::value,
          void>::type
        construct_from_args(
          Alloc& alloc, std::pair<A, B>* address, A0&&, A1&& a1, A2&& a2)
        {
          boost::allocator_construct(alloc, address, std::piecewise_construct,
            to_std_tuple(a1), to_std_tuple(a2));
        }
      } // namespace func
    } // namespace detail
  } // namespace unordered
} // namespace boost

namespace boost {
  namespace unordered {
    namespace detail {

      ///////////////////////////////////////////////////////////////////
      //
      // Node construction

      template <typename NodeAlloc> struct node_constructor
      {
        typedef NodeAlloc node_allocator;
        typedef boost::unordered::detail::allocator_traits<NodeAlloc>
          node_allocator_traits;
        typedef typename node_allocator_traits::value_type node;
        typedef typename node_allocator_traits::pointer node_pointer;
        typedef typename node::value_type value_type;

        node_allocator& alloc_;
        node_pointer node_;

        node_constructor(node_allocator& n) : alloc_(n), node_() {}

        ~node_constructor();

        void create_node();

        // no throw
        node_pointer release()
        {
          BOOST_ASSERT(node_);
          node_pointer p = node_;
          node_ = node_pointer();
          return p;
        }

      private:
        node_constructor(node_constructor const&);
        node_constructor& operator=(node_constructor const&);
      };

      template <typename Alloc> node_constructor<Alloc>::~node_constructor()
      {
        if (node_) {
          boost::unordered::detail::func::destroy(boost::to_address(node_));
          node_allocator_traits::deallocate(alloc_, node_, 1);
        }
      }

      template <typename Alloc> void node_constructor<Alloc>::create_node()
      {
        BOOST_ASSERT(!node_);
        node_ = node_allocator_traits::allocate(alloc_, 1);
        new ((void*)boost::to_address(node_)) node();
      }

      template <typename NodeAlloc> struct node_tmp
      {
        typedef typename boost::allocator_value_type<NodeAlloc>::type node;
        typedef typename boost::allocator_pointer<NodeAlloc>::type node_pointer;
        typedef typename node::value_type value_type;
        typedef typename boost::allocator_rebind<NodeAlloc, value_type>::type
          value_allocator;

        NodeAlloc& alloc_;
        node_pointer node_;

        explicit node_tmp(node_pointer n, NodeAlloc& a) : alloc_(a), node_(n) {}

        ~node_tmp();

        // no throw
        node_pointer release()
        {
          node_pointer p = node_;
          node_ = node_pointer();
          return p;
        }
      };

      template <typename Alloc> node_tmp<Alloc>::~node_tmp()
      {
        if (node_) {
          value_allocator val_alloc(alloc_);
          boost::allocator_destroy(val_alloc, node_->value_ptr());
          boost::allocator_deallocate(alloc_, node_, 1);
        }
      }
    } // namespace detail
  } // namespace unordered
} // namespace boost

namespace boost {
  namespace unordered {
    namespace detail {
      namespace func {

        // Some nicer construct_node functions, might try to
        // improve implementation later.

        template <typename Alloc, typename... Args>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_from_args(Alloc& alloc, Args&&... args)
        {
          typedef typename boost::allocator_value_type<Alloc>::type node;
          typedef typename node::value_type value_type;
          typedef typename boost::allocator_rebind<Alloc, value_type>::type
            value_allocator;

          value_allocator val_alloc(alloc);

          node_constructor<Alloc> a(alloc);
          a.create_node();
          construct_from_args(
            val_alloc, a.node_->value_ptr(), std::forward<Args>(args)...);
          return a.release();
        }

        template <typename Alloc, typename U>
        inline typename boost::allocator_pointer<Alloc>::type construct_node(
          Alloc& alloc, U&& x)
        {
          node_constructor<Alloc> a(alloc);
          a.create_node();

          typedef typename boost::allocator_value_type<Alloc>::type node;
          typedef typename node::value_type value_type;
          typedef typename boost::allocator_rebind<Alloc, value_type>::type
            value_allocator;

          value_allocator val_alloc(alloc);

          boost::allocator_construct(
            val_alloc, a.node_->value_ptr(), std::forward<U>(x));
          return a.release();
        }

        template <typename Alloc, typename Key>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_pair(Alloc& alloc, Key&& k)
        {
          node_constructor<Alloc> a(alloc);
          a.create_node();

          typedef typename boost::allocator_value_type<Alloc>::type node;
          typedef typename node::value_type value_type;
          typedef typename boost::allocator_rebind<Alloc, value_type>::type
            value_allocator;

          value_allocator val_alloc(alloc);

          boost::allocator_construct(val_alloc, a.node_->value_ptr(),
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<Key>(k)),
            std::forward_as_tuple());
          return a.release();
        }

        template <typename Alloc, typename Key, typename Mapped>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_pair(Alloc& alloc, Key&& k, Mapped&& m)
        {
          node_constructor<Alloc> a(alloc);
          a.create_node();

          typedef typename boost::allocator_value_type<Alloc>::type node;
          typedef typename node::value_type value_type;
          typedef typename boost::allocator_rebind<Alloc, value_type>::type
            value_allocator;

          value_allocator val_alloc(alloc);

          boost::allocator_construct(val_alloc, a.node_->value_ptr(),
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<Key>(k)),
            std::forward_as_tuple(std::forward<Mapped>(m)));
          return a.release();
        }

        template <typename Alloc, typename Key, typename... Args>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_pair_from_args(Alloc& alloc, Key&& k, Args&&... args)
        {
          node_constructor<Alloc> a(alloc);
          a.create_node();

          typedef typename boost::allocator_value_type<Alloc>::type node;
          typedef typename node::value_type value_type;
          typedef typename boost::allocator_rebind<Alloc, value_type>::type
            value_allocator;

          value_allocator val_alloc(alloc);

          boost::allocator_construct(val_alloc, a.node_->value_ptr(),
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<Key>(k)),
            std::forward_as_tuple(std::forward<Args>(args)...));

          return a.release();
        }

        template <typename T, typename Alloc, typename Key>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_from_key(T*, Alloc& alloc, Key&& k)
        {
          return construct_node(alloc, std::forward<Key>(k));
        }

        template <typename T, typename V, typename Alloc, typename Key>
        inline typename boost::allocator_pointer<Alloc>::type
        construct_node_from_key(std::pair<T const, V>*, Alloc& alloc, Key&& k)
        {
          return construct_node_pair(alloc, std::forward<Key>(k));
        }
      } // namespace func
    } // namespace detail
  } // namespace unordered
} // namespace boost

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

namespace boost {
  namespace unordered {
    namespace detail {
      //////////////////////////////////////////////////////////////////////////
      // Functions
      //
      // This double buffers the storage for the hash function and key equality
      // predicate in order to have exception safe copy/swap. To do so,
      // use 'construct_spare' to construct in the spare space, and then when
      // ready to use 'switch_functions' to switch to the new functions.
      // If an exception is thrown between these two calls, use
      // 'cleanup_spare_functions' to destroy the unused constructed functions.

#if defined(_GLIBCXX_HAVE_BUILTIN_LAUNDER)
      // gcc-12 warns when accessing the `current_functions` of our `functions`
      // class below with `-Wmaybe-unitialized`. By laundering the pointer, we
      // silence the warning and assure the compiler that a valid object exists
      // in that region of storage. This warning is also generated in C++03
      // which does not have `std::launder`. The compiler builtin is always
      // available, regardless of the C++ standard used when compiling.
      template <class T> T* launder(T* p) noexcept
      {
        return __builtin_launder(p);
      }
#else
      template <class T> T* launder(T* p) noexcept { return p; }
#endif

      template <class H, class P> class functions
      {
      public:
        static const bool nothrow_move_assignable =
          std::is_nothrow_move_assignable<H>::value &&
          std::is_nothrow_move_assignable<P>::value;
        static const bool nothrow_move_constructible =
          std::is_nothrow_move_constructible<H>::value &&
          std::is_nothrow_move_constructible<P>::value;
        static const bool nothrow_swappable =
          boost::unordered::detail::is_nothrow_swappable<H>::value &&
          boost::unordered::detail::is_nothrow_swappable<P>::value;

      private:
        functions& operator=(functions const&);

        typedef compressed<H, P> function_pair;

        unsigned char current_; // 0/1 - Currently active functions
                                // +2 - Both constructed
        opt_storage<function_pair> funcs_[2];

      public:
        functions(H const& hf, P const& eq) : current_(0)
        {
          construct_functions(current_, hf, eq);
        }

        functions(functions const& bf) : current_(0)
        {
          construct_functions(current_, bf.current_functions());
        }

        functions(functions& bf, boost::unordered::detail::move_tag)
            : current_(0)
        {
          construct_functions(current_, bf.current_functions(),
            std::integral_constant<bool, nothrow_move_constructible>());
        }

        ~functions()
        {
          BOOST_ASSERT(!(current_ & 2));
          destroy_functions(current_);
        }

        H const& hash_function() const { return current_functions().first(); }

        P const& key_eq() const { return current_functions().second(); }

        function_pair const& current_functions() const
        {
          return *::boost::unordered::detail::launder(
            static_cast<function_pair const*>(
              static_cast<void const*>(funcs_[current_ & 1].address())));
        }

        function_pair& current_functions()
        {
          return *::boost::unordered::detail::launder(
            static_cast<function_pair*>(
              static_cast<void*>(funcs_[current_ & 1].address())));
        }

        void construct_spare_functions(function_pair const& f)
        {
          BOOST_ASSERT(!(current_ & 2));
          construct_functions(current_ ^ 1, f);
          current_ |= 2;
        }

        void cleanup_spare_functions()
        {
          if (current_ & 2) {
            current_ = static_cast<unsigned char>(current_ & 1);
            destroy_functions(current_ ^ 1);
          }
        }

        void switch_functions()
        {
          BOOST_ASSERT(current_ & 2);
          destroy_functions(static_cast<unsigned char>(current_ & 1));
          current_ ^= 3;
        }

      private:
        void construct_functions(unsigned char which, H const& hf, P const& eq)
        {
          BOOST_ASSERT(!(which & 2));
          new ((void*)&funcs_[which]) function_pair(hf, eq);
        }

        void construct_functions(
          unsigned char which, function_pair const& f, std::false_type = {})
        {
          BOOST_ASSERT(!(which & 2));
          new ((void*)&funcs_[which]) function_pair(f);
        }

        void construct_functions(
          unsigned char which, function_pair& f, std::true_type)
        {
          BOOST_ASSERT(!(which & 2));
          new ((void*)&funcs_[which])
            function_pair(f, boost::unordered::detail::move_tag());
        }

        void destroy_functions(unsigned char which)
        {
          BOOST_ASSERT(!(which & 2));
          boost::unordered::detail::func::destroy(
            (function_pair*)(&funcs_[which]));
        }
      };

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable : 4127) // conditional expression is constant
#endif

      //////////////////////////////////////////////////////////////////////////
      // convert double to std::size_t

      inline std::size_t double_to_size(double f)
      {
        return f >= static_cast<double>(
                      (std::numeric_limits<std::size_t>::max)())
                 ? (std::numeric_limits<std::size_t>::max)()
                 : static_cast<std::size_t>(f);
      }

      //////////////////////////////////////////////////////////////////////////
      // iterator definitions

      namespace iterator_detail {
        template <class Node, class Bucket> class c_iterator;

        template <class Node, class Bucket> class iterator
        {
        public:
          typedef typename Node::value_type value_type;
          typedef value_type element_type;
          typedef value_type* pointer;
          typedef value_type& reference;
          typedef std::ptrdiff_t difference_type;
          typedef std::forward_iterator_tag iterator_category;

          iterator() : p(), itb() {}

          reference operator*() const noexcept { return dereference(); }
          pointer operator->() const noexcept
          {
            pointer x = std::addressof(p->value());
            return x;
          }

          iterator& operator++() noexcept
          {
            increment();
            return *this;
          }

          iterator operator++(int) noexcept
          {
            iterator old = *this;
            increment();
            return old;
          }

          bool operator==(iterator const& other) const noexcept
          {
            return equal(other);
          }

          bool operator!=(iterator const& other) const noexcept
          {
            return !equal(other);
          }

          bool operator==(
            boost::unordered::detail::iterator_detail::c_iterator<Node,
              Bucket> const& other) const noexcept
          {
            return equal(other);
          }

          bool operator!=(
            boost::unordered::detail::iterator_detail::c_iterator<Node,
              Bucket> const& other) const noexcept
          {
            return !equal(other);
          }

        private:
          typedef typename Node::node_pointer node_pointer;
          typedef grouped_bucket_iterator<Bucket> bucket_iterator;

          node_pointer p;
          bucket_iterator itb;

          template <class Types> friend struct boost::unordered::detail::table;
          template <class N, class B> friend class c_iterator;

          iterator(node_pointer p_, bucket_iterator itb_) : p(p_), itb(itb_) {}

          value_type& dereference() const noexcept { return p->value(); }

          bool equal(const iterator& x) const noexcept { return (p == x.p); }

          bool equal(
            const boost::unordered::detail::iterator_detail::c_iterator<Node,
              Bucket>& x) const noexcept
          {
            return (p == x.p);
          }

          void increment() noexcept
          {
            p = p->next;
            if (!p) {
              p = (++itb)->next;
            }
          }

          template <typename Archive>
          friend void serialization_track(Archive& ar, const iterator& x)
          {
            if (x.p) {
              track_address(ar, x.p);
              serialization_track(ar, x.itb);
            }
          }

          friend class boost::serialization::access;

          template <typename Archive> void serialize(Archive& ar, unsigned int)
          {
            if (!p)
              itb = bucket_iterator();
            serialize_tracked_address(ar, p);
            ar& core::make_nvp("bucket_iterator", itb);
          }
        };

        template <class Node, class Bucket> class c_iterator
        {
        public:
          typedef typename Node::value_type value_type;
          typedef value_type const element_type;
          typedef value_type const* pointer;
          typedef value_type const& reference;
          typedef std::ptrdiff_t difference_type;
          typedef std::forward_iterator_tag iterator_category;

          c_iterator() : p(), itb() {}
          c_iterator(iterator<Node, Bucket> it) : p(it.p), itb(it.itb) {}

          reference operator*() const noexcept { return dereference(); }
          pointer operator->() const noexcept
          {
            pointer x = std::addressof(p->value());
            return x;
          }

          c_iterator& operator++() noexcept
          {
            increment();
            return *this;
          }

          c_iterator operator++(int) noexcept
          {
            c_iterator old = *this;
            increment();
            return old;
          }

          bool operator==(c_iterator const& other) const noexcept
          {
            return equal(other);
          }

          bool operator!=(c_iterator const& other) const noexcept
          {
            return !equal(other);
          }

          bool operator==(
            boost::unordered::detail::iterator_detail::iterator<Node,
              Bucket> const& other) const noexcept
          {
            return equal(other);
          }

          bool operator!=(
            boost::unordered::detail::iterator_detail::iterator<Node,
              Bucket> const& other) const noexcept
          {
            return !equal(other);
          }

        private:
          typedef typename Node::node_pointer node_pointer;
          typedef grouped_bucket_iterator<Bucket> bucket_iterator;

          node_pointer p;
          bucket_iterator itb;

          template <class Types> friend struct boost::unordered::detail::table;
          template <class, class> friend class iterator;

          c_iterator(node_pointer p_, bucket_iterator itb_) : p(p_), itb(itb_)
          {
          }

          value_type const& dereference() const noexcept { return p->value(); }

          bool equal(const c_iterator& x) const noexcept { return (p == x.p); }

          void increment() noexcept
          {
            p = p->next;
            if (!p) {
              p = (++itb)->next;
            }
          }

          template <typename Archive>
          friend void serialization_track(Archive& ar, const c_iterator& x)
          {
            if (x.p) {
              track_address(ar, x.p);
              serialization_track(ar, x.itb);
            }
          }

          friend class boost::serialization::access;

          template <typename Archive> void serialize(Archive& ar, unsigned int)
          {
            if (!p)
              itb = bucket_iterator();
            serialize_tracked_address(ar, p);
            ar& core::make_nvp("bucket_iterator", itb);
          }
        };
      } // namespace iterator_detail

      //////////////////////////////////////////////////////////////////////////
      // table structure used by the containers
      template <typename Types>
      struct table : boost::unordered::detail::functions<typename Types::hasher,
                       typename Types::key_equal>
      {
      private:
        table(table const&);
        table& operator=(table const&);

      public:
        typedef typename Types::hasher hasher;
        typedef typename Types::key_equal key_equal;
        typedef typename Types::const_key_type const_key_type;
        typedef typename Types::extractor extractor;
        typedef typename Types::value_type value_type;
        typedef typename Types::table table_impl;

        typedef boost::unordered::detail::functions<typename Types::hasher,
          typename Types::key_equal>
          functions;

        typedef typename Types::value_allocator value_allocator;
        typedef typename boost::allocator_void_pointer<value_allocator>::type
          void_pointer;
        typedef node<value_type, void_pointer> node_type;

        typedef boost::unordered::detail::grouped_bucket_array<
          bucket<node_type, void_pointer>, value_allocator, prime_fmod_size<> >
          bucket_array_type;

        typedef
          typename bucket_array_type::node_allocator_type node_allocator_type;
        typedef typename boost::allocator_pointer<node_allocator_type>::type
          node_pointer;

        typedef boost::unordered::detail::node_constructor<node_allocator_type>
          node_constructor;
        typedef boost::unordered::detail::node_tmp<node_allocator_type>
          node_tmp;

        typedef typename bucket_array_type::bucket_type bucket_type;

        typedef typename bucket_array_type::iterator bucket_iterator;

        typedef typename bucket_array_type::local_iterator l_iterator;
        typedef typename bucket_array_type::const_local_iterator cl_iterator;

        typedef std::size_t size_type;

        typedef iterator_detail::iterator<node_type, bucket_type> iterator;
        typedef iterator_detail::c_iterator<node_type, bucket_type> c_iterator;

        typedef std::pair<iterator, bool> emplace_return;

        ////////////////////////////////////////////////////////////////////////
        // Members

        std::size_t size_;
        float mlf_;
        std::size_t max_load_;
        bucket_array_type buckets_;

      public:
        ////////////////////////////////////////////////////////////////////////
        // Data access

        size_type bucket_count() const { return buckets_.bucket_count(); }

        template <class Key>
        iterator next_group(Key const& k, c_iterator n) const
        {
          c_iterator last = this->end();
          while (n != last && this->key_eq()(k, extractor::extract(*n))) {
            ++n;
          }
          return iterator(n.p, n.itb);
        }

        template <class Key> std::size_t group_count(Key const& k) const
        {
          if (size_ == 0) {
            return 0;
          }
          std::size_t c = 0;
          std::size_t const key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

          bool found = false;

          for (node_pointer pos = itb->next; pos; pos = pos->next) {
            if (this->key_eq()(k, this->get_key(pos))) {
              ++c;
              found = true;
            } else if (found) {
              break;
            }
          }
          return c;
        }

        node_allocator_type const& node_alloc() const
        {
          return buckets_.get_node_allocator();
        }

        node_allocator_type& node_alloc()
        {
          return buckets_.get_node_allocator();
        }

        std::size_t max_bucket_count() const
        {
          typedef typename bucket_array_type::size_policy size_policy;
          return size_policy::size(size_policy::size_index(
            boost::allocator_max_size(this->node_alloc())));
        }

        iterator begin() const
        {
          if (size_ == 0) {
            return end();
          }

          bucket_iterator itb = buckets_.begin();
          return iterator(itb->next, itb);
        }

        iterator end() const { return iterator(); }

        l_iterator begin(std::size_t bucket_index) const
        {
          return buckets_.begin(bucket_index);
        }

        std::size_t hash_to_bucket(std::size_t hash_value) const
        {
          return buckets_.position(hash_value);
        }

        std::size_t bucket_size(std::size_t index) const
        {
          std::size_t count = 0;
          if (size_ > 0) {
            bucket_iterator itb = buckets_.at(index);
            node_pointer n = itb->next;
            while (n) {
              ++count;
              n = n->next;
            }
          }
          return count;
        }

        ////////////////////////////////////////////////////////////////////////
        // Load methods

        void recalculate_max_load()
        {
          // From 6.3.1/13:
          // Only resize when size >= mlf_ * count
          std::size_t const bc = buckets_.bucket_count();

          // it's important we do the `bc == 0` check here because the `mlf_`
          // can be specified to be infinity. The operation `n * INF` is `INF`
          // for all `n > 0` but NaN for `n == 0`.
          //
          max_load_ =
            bc == 0 ? 0
                    : boost::unordered::detail::double_to_size(
                        static_cast<double>(mlf_) * static_cast<double>(bc));
        }

        void max_load_factor(float z)
        {
          BOOST_ASSERT(z > 0);
          mlf_ = (std::max)(z, minimum_max_load_factor);
          recalculate_max_load();
        }

        ////////////////////////////////////////////////////////////////////////
        // Constructors

        table()
            : functions(hasher(), key_equal()), size_(0), mlf_(1.0f),
              max_load_(0)
        {
        }

        table(std::size_t num_buckets, hasher const& hf, key_equal const& eq,
          value_allocator const& a)
            : functions(hf, eq), size_(0), mlf_(1.0f), max_load_(0),
              buckets_(num_buckets, a)
        {
          recalculate_max_load();
        }

        table(table const& x, value_allocator const& a)
            : functions(x), size_(0), mlf_(x.mlf_), max_load_(0),
              buckets_(x.size_, a)
        {
          recalculate_max_load();
        }

        table(table& x, boost::unordered::detail::move_tag m)
            : functions(x, m), size_(x.size_), mlf_(x.mlf_),
              max_load_(x.max_load_), buckets_(std::move(x.buckets_))
        {
          x.size_ = 0;
          x.max_load_ = 0;
        }

        table(table& x, value_allocator const& a,
          boost::unordered::detail::move_tag m)
            : functions(x, m), size_(0), mlf_(x.mlf_), max_load_(0),
              buckets_(x.bucket_count(), a)
        {
          recalculate_max_load();
        }

        ////////////////////////////////////////////////////////////////////////
        // Swap and Move

        void swap_allocators(table& other, std::false_type)
        {
          boost::unordered::detail::func::ignore_unused_variable_warning(other);

          // According to 23.2.1.8, if propagate_on_container_swap is
          // false the behaviour is undefined unless the allocators
          // are equal.
          BOOST_ASSERT(node_alloc() == other.node_alloc());
        }

        // Not nothrow swappable
        void swap(table& x, std::false_type)
        {
          if (this == &x) {
            return;
          }

          this->construct_spare_functions(x.current_functions());
          BOOST_TRY { x.construct_spare_functions(this->current_functions()); }
          BOOST_CATCH(...)
          {
            this->cleanup_spare_functions();
            BOOST_RETHROW
          }
          BOOST_CATCH_END
          this->switch_functions();
          x.switch_functions();

          buckets_.swap(x.buckets_);
          boost::core::invoke_swap(size_, x.size_);
          std::swap(mlf_, x.mlf_);
          std::swap(max_load_, x.max_load_);
        }

        // Nothrow swappable
        void swap(table& x, std::true_type)
        {
          buckets_.swap(x.buckets_);
          boost::core::invoke_swap(size_, x.size_);
          std::swap(mlf_, x.mlf_);
          std::swap(max_load_, x.max_load_);
          this->current_functions().swap(x.current_functions());
        }

        // Only swaps the allocators if propagate_on_container_swap.
        // If not propagate_on_container_swap and allocators aren't
        // equal, behaviour is undefined.
        void swap(table& x)
        {
          BOOST_ASSERT(boost::allocator_propagate_on_container_swap<
                         node_allocator_type>::type::value ||
                       node_alloc() == x.node_alloc());
          swap(x, std::integral_constant<bool, functions::nothrow_swappable>());
        }

        // Only call with nodes allocated with the currect allocator, or
        // one that is equal to it. (Can't assert because other's
        // allocators might have already been moved).
        void move_buckets_from(table& other)
        {
          buckets_ = std::move(other.buckets_);

          size_ = other.size_;
          max_load_ = other.max_load_;

          other.size_ = 0;
          other.max_load_ = 0;
        }

        // For use in the constructor when allocators might be different.
        void move_construct_buckets(table& src)
        {
          if (this->node_alloc() == src.node_alloc()) {
            move_buckets_from(src);
            return;
          }

          if (src.size_ == 0) {
            return;
          }

          BOOST_ASSERT(buckets_.bucket_count() == src.buckets_.bucket_count());

          this->reserve(src.size_);
          for (iterator pos = src.begin(); pos != src.end(); ++pos) {
            node_tmp b(detail::func::construct_node(
                         this->node_alloc(), std::move(pos.p->value())),
              this->node_alloc());

            const_key_type& k = this->get_key(b.node_);
            std::size_t key_hash = this->hash(k);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
            buckets_.insert_node(itb, b.release());
            ++size_;
          }
        }

        ////////////////////////////////////////////////////////////////////////
        // Delete/destruct

        ~table() { delete_buckets(); }

        void delete_node(node_pointer p)
        {
          node_allocator_type alloc = this->node_alloc();

          value_allocator val_alloc(alloc);
          boost::allocator_destroy(val_alloc, p->value_ptr());
          boost::unordered::detail::func::destroy(boost::to_address(p));
          boost::allocator_deallocate(alloc, p, 1);
        }

        void delete_buckets()
        {
          iterator pos = begin(), last = this->end();
          for (; pos != last;) {
            node_pointer p = pos.p;
            bucket_iterator itb = pos.itb;
            ++pos;
            buckets_.extract_node(itb, p);
            delete_node(p);
            --size_;
          }

          buckets_.clear();
        }

        ////////////////////////////////////////////////////////////////////////
        // Clear

        void clear_impl();

        ////////////////////////////////////////////////////////////////////////
        // Assignment

        template <typename UniqueType>
        void assign(table const& x, UniqueType is_unique)
        {
          typedef
            typename boost::allocator_propagate_on_container_copy_assignment<
              node_allocator_type>::type pocca;

          if (this != &x) {
            assign(x, is_unique, std::integral_constant<bool, pocca::value>());
          }
        }

        template <typename UniqueType>
        void assign(table const& x, UniqueType is_unique, std::false_type)
        {
          // Strong exception safety.
          this->construct_spare_functions(x.current_functions());
          BOOST_TRY
          {
            mlf_ = x.mlf_;
            recalculate_max_load();

            this->reserve_for_insert(x.size_);
            this->clear_impl();
          }
          BOOST_CATCH(...)
          {
            this->cleanup_spare_functions();
            BOOST_RETHROW
          }
          BOOST_CATCH_END
          this->switch_functions();
          copy_buckets(x, is_unique);
        }

        template <typename UniqueType>
        void assign(table const& x, UniqueType is_unique, std::true_type)
        {
          if (node_alloc() == x.node_alloc()) {
            buckets_.reset_allocator(x.node_alloc());
            assign(x, is_unique, std::false_type());
          } else {
            bucket_array_type new_buckets(x.size_, x.node_alloc());
            this->construct_spare_functions(x.current_functions());
            this->switch_functions();

            // Delete everything with current allocators before assigning
            // the new ones.
            delete_buckets();
            buckets_.reset_allocator(x.node_alloc());
            buckets_ = std::move(new_buckets);

            // Copy over other data, all no throw.
            mlf_ = x.mlf_;
            reserve(x.size_);

            // Finally copy the elements.
            if (x.size_) {
              copy_buckets(x, is_unique);
            }
          }
        }

        template <typename UniqueType>
        void move_assign(table& x, UniqueType is_unique)
        {
          if (this != &x) {
            move_assign(x, is_unique,
              std::integral_constant<bool,
                boost::allocator_propagate_on_container_move_assignment<
                  node_allocator_type>::type::value>());
          }
        }

        // Propagate allocator
        template <typename UniqueType>
        void move_assign(table& x, UniqueType, std::true_type)
        {
          if (!functions::nothrow_move_assignable) {
            this->construct_spare_functions(x.current_functions());
            this->switch_functions();
          } else {
            this->current_functions().move_assign(x.current_functions());
          }
          delete_buckets();

          buckets_.reset_allocator(x.buckets_.get_node_allocator());
          mlf_ = x.mlf_;
          move_buckets_from(x);
        }

        // Don't propagate allocator
        template <typename UniqueType>
        void move_assign(table& x, UniqueType is_unique, std::false_type)
        {
          if (node_alloc() == x.node_alloc()) {
            move_assign_equal_alloc(x);
          } else {
            move_assign_realloc(x, is_unique);
          }
        }

        void move_assign_equal_alloc(table& x)
        {
          if (!functions::nothrow_move_assignable) {
            this->construct_spare_functions(x.current_functions());
            this->switch_functions();
          } else {
            this->current_functions().move_assign(x.current_functions());
          }
          delete_buckets();
          mlf_ = x.mlf_;
          move_buckets_from(x);
        }

        template <typename UniqueType>
        void move_assign_realloc(table& x, UniqueType is_unique)
        {
          this->construct_spare_functions(x.current_functions());
          BOOST_TRY
          {
            mlf_ = x.mlf_;
            recalculate_max_load();
            if (x.size_ > 0) {
              this->reserve_for_insert(x.size_);
            }
            this->clear_impl();
          }
          BOOST_CATCH(...)
          {
            this->cleanup_spare_functions();
            BOOST_RETHROW
          }
          BOOST_CATCH_END
          this->switch_functions();
          move_assign_buckets(x, is_unique);
        }

        // Accessors

        const_key_type& get_key(node_pointer n) const
        {
          return extractor::extract(n->value());
        }

        template <class Key> std::size_t hash(Key const& k) const
        {
          return this->hash_function()(k);
        }

        // Find Node

        template <class Key>
        node_pointer find_node_impl(Key const& x, bucket_iterator itb) const
        {
          node_pointer p = node_pointer();
          if (itb != buckets_.end()) {
            key_equal const& pred = this->key_eq();
            p = itb->next;
            for (; p; p = p->next) {
              if (pred(x, extractor::extract(p->value()))) {
                break;
              }
            }
          }
          return p;
        }

        template <class Key> node_pointer find_node(Key const& k) const
        {
          std::size_t const key_hash = this->hash(k);
          return find_node_impl(k, buckets_.at(buckets_.position(key_hash)));
        }

        node_pointer find_node(const_key_type& k, bucket_iterator itb) const
        {
          return find_node_impl(k, itb);
        }

        template <class Key> iterator find(Key const& k) const
        {
          return this->transparent_find(
            k, this->hash_function(), this->key_eq());
        }

        template <class Key, class Hash, class Pred>
        inline iterator transparent_find(
          Key const& k, Hash const& h, Pred const& pred) const
        {
          if (size_ > 0) {
            std::size_t const key_hash = h(k);
            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
            for (node_pointer p = itb->next; p; p = p->next) {
              if (BOOST_LIKELY(pred(k, extractor::extract(p->value())))) {
                return iterator(p, itb);
              }
            }
          }

          return this->end();
        }

        template <class Key>
        node_pointer* find_prev(Key const& key, bucket_iterator itb)
        {
          if (size_ > 0) {
            key_equal pred = this->key_eq();
            for (node_pointer* pp = std::addressof(itb->next); *pp;
                 pp = std::addressof((*pp)->next)) {
              if (pred(key, extractor::extract((*pp)->value()))) {
                return pp;
              }
            }
          }
          typedef node_pointer* node_pointer_pointer;
          return node_pointer_pointer();
        }

        // Extract and erase

        template <class Key> node_pointer extract_by_key_impl(Key const& k)
        {
          iterator it = this->find(k);
          if (it == this->end()) {
            return node_pointer();
          }

          buckets_.extract_node(it.itb, it.p);
          --size_;

          return it.p;
        }

        // Reserve and rehash
        void transfer_node(
          node_pointer p, bucket_type&, bucket_array_type& new_buckets)
        {
          const_key_type& key = extractor::extract(p->value());
          std::size_t const h = this->hash(key);
          bucket_iterator itnewb = new_buckets.at(new_buckets.position(h));
          new_buckets.insert_node(itnewb, p);
        }

        static std::size_t min_buckets(std::size_t num_elements, float mlf)
        {
          std::size_t num_buckets = static_cast<std::size_t>(
            std::ceil(static_cast<float>(num_elements) / mlf));

          if (num_buckets == 0 && num_elements > 0) { // mlf == inf
            num_buckets = 1;
          }
          return num_buckets;
        }

        void rehash(std::size_t);
        void reserve(std::size_t);
        void reserve_for_insert(std::size_t);
        void rehash_impl(std::size_t);

        ////////////////////////////////////////////////////////////////////////
        // Unique keys

        // equals

        bool equals_unique(table const& other) const
        {
          if (this->size_ != other.size_)
            return false;

          c_iterator pos = this->begin();
          c_iterator last = this->end();

          while (pos != last) {
            node_pointer p = pos.p;
            node_pointer p2 = other.find_node(this->get_key(p));
            if (!p2 || !(p->value() == p2->value())) {
              return false;
            }
            ++pos;
          }

          return true;
        }

        // Emplace/Insert

        template <typename... Args>
        iterator emplace_hint_unique(
          c_iterator hint, const_key_type& k, Args&&... args)
        {
          if (hint.p && this->key_eq()(k, this->get_key(hint.p))) {
            return iterator(hint.p, hint.itb);
          } else {
            return emplace_unique(k, std::forward<Args>(args)...).first;
          }
        }

        template <typename... Args>
        emplace_return emplace_unique(const_key_type& k, Args&&... args)
        {
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer pos = this->find_node_impl(k, itb);

          if (pos) {
            return emplace_return(iterator(pos, itb), false);
          } else {
            node_tmp b(boost::unordered::detail::func::construct_node_from_args(
                         this->node_alloc(), std::forward<Args>(args)...),
              this->node_alloc());

            if (size_ + 1 > max_load_) {
              reserve(size_ + 1);
              itb = buckets_.at(buckets_.position(key_hash));
            }

            node_pointer p = b.release();
            buckets_.insert_node(itb, p);
            ++size_;

            return emplace_return(iterator(p, itb), true);
          }
        }

        template <typename... Args>
        iterator emplace_hint_unique(c_iterator hint, no_key, Args&&... args)
        {
          node_tmp b(boost::unordered::detail::func::construct_node_from_args(
                       this->node_alloc(), std::forward<Args>(args)...),
            this->node_alloc());

          const_key_type& k = this->get_key(b.node_);
          if (hint.p && this->key_eq()(k, this->get_key(hint.p))) {
            return iterator(hint.p, hint.itb);
          }

          std::size_t const key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

          node_pointer p = this->find_node_impl(k, itb);
          if (p) {
            return iterator(p, itb);
          }

          if (size_ + 1 > max_load_) {
            this->reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }

          p = b.release();
          buckets_.insert_node(itb, p);
          ++size_;
          return iterator(p, itb);
        }

        template <typename... Args>
        emplace_return emplace_unique(no_key, Args&&... args)
        {
          node_tmp b(boost::unordered::detail::func::construct_node_from_args(
                       this->node_alloc(), std::forward<Args>(args)...),
            this->node_alloc());

          const_key_type& k = this->get_key(b.node_);
          std::size_t key_hash = this->hash(k);

          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer pos = this->find_node_impl(k, itb);

          if (pos) {
            return emplace_return(iterator(pos, itb), false);
          } else {
            if (size_ + 1 > max_load_) {
              reserve(size_ + 1);
              itb = buckets_.at(buckets_.position(key_hash));
            }

            node_pointer p = b.release();
            buckets_.insert_node(itb, p);
            ++size_;

            return emplace_return(iterator(p, itb), true);
          }
        }

        template <typename K, typename V>
        emplace_return emplace_unique(converting_key, K&& k, V&& v)
        {
          using alloc_cted = allocator_constructed<node_allocator_type,
            typename Types::key_type>;
          alloc_cted key(this->node_alloc(), std::forward<K>(k));
          return emplace_unique(
            key.value(), std::move(key.value()), std::forward<V>(v));
        }

        template <typename Key> emplace_return try_emplace_unique(Key&& k)
        {
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

          node_pointer pos = this->find_node_impl(k, itb);

          if (pos) {
            return emplace_return(iterator(pos, itb), false);
          } else {
            node_allocator_type alloc = node_alloc();

            value_type* dispatch = BOOST_NULLPTR;

            node_tmp tmp(detail::func::construct_node_from_key(
                           dispatch, alloc, std::forward<Key>(k)),
              alloc);

            if (size_ + 1 > max_load_) {
              reserve(size_ + 1);
              itb = buckets_.at(buckets_.position(key_hash));
            }

            node_pointer p = tmp.release();
            buckets_.insert_node(itb, p);

            ++size_;
            return emplace_return(iterator(p, itb), true);
          }
        }

        template <typename Key>
        iterator try_emplace_hint_unique(c_iterator hint, Key&& k)
        {
          if (hint.p && this->key_eq()(extractor::extract(*hint), k)) {
            return iterator(hint.p, hint.itb);
          } else {
            return try_emplace_unique(k).first;
          }
        }

        template <typename Key, typename... Args>
        emplace_return try_emplace_unique(Key&& k, Args&&... args)
        {
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

          node_pointer pos = this->find_node_impl(k, itb);

          if (pos) {
            return emplace_return(iterator(pos, itb), false);
          }

          node_tmp b(
            boost::unordered::detail::func::construct_node_pair_from_args(
              this->node_alloc(), k, std::forward<Args>(args)...),
            this->node_alloc());

          if (size_ + 1 > max_load_) {
            reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }

          pos = b.release();

          buckets_.insert_node(itb, pos);
          ++size_;
          return emplace_return(iterator(pos, itb), true);
        }

        template <typename Key, typename... Args>
        iterator try_emplace_hint_unique(
          c_iterator hint, Key&& k, Args&&... args)
        {
          if (hint.p && this->key_eq()(hint->first, k)) {
            return iterator(hint.p, hint.itb);
          } else {
            return try_emplace_unique(k, std::forward<Args>(args)...).first;
          }
        }

        template <typename Key, typename M>
        emplace_return insert_or_assign_unique(Key&& k, M&& obj)
        {
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

          node_pointer p = this->find_node_impl(k, itb);
          if (p) {
            p->value().second = std::forward<M>(obj);
            return emplace_return(iterator(p, itb), false);
          }

          node_tmp b(
            boost::unordered::detail::func::construct_node_pair(
              this->node_alloc(), std::forward<Key>(k), std::forward<M>(obj)),
            node_alloc());

          if (size_ + 1 > max_load_) {
            reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }

          p = b.release();

          buckets_.insert_node(itb, p);
          ++size_;
          return emplace_return(iterator(p, itb), true);
        }

        template <typename NodeType, typename InsertReturnType>
        void move_insert_node_type_unique(
          NodeType& np, InsertReturnType& result)
        {
          if (!np) {
            result.position = this->end();
            result.inserted = false;
            return;
          }

          const_key_type& k = this->get_key(np.ptr_);
          std::size_t const key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer p = this->find_node_impl(k, itb);

          if (p) {
            iterator pos(p, itb);
            result.node = std::move(np);
            result.position = pos;
            result.inserted = false;
            return;
          }

          this->reserve_for_insert(size_ + 1);

          p = np.ptr_;
          itb = buckets_.at(buckets_.position(key_hash));

          buckets_.insert_node(itb, p);
          np.ptr_ = node_pointer();
          ++size_;

          result.position = iterator(p, itb);
          result.inserted = true;
        }

        template <typename NodeType>
        iterator move_insert_node_type_with_hint_unique(
          c_iterator hint, NodeType& np)
        {
          if (!np) {
            return this->end();
          }

          const_key_type& k = this->get_key(np.ptr_);
          if (hint.p && this->key_eq()(k, this->get_key(hint.p))) {
            return iterator(hint.p, hint.itb);
          }

          std::size_t const key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer p = this->find_node_impl(k, itb);
          if (p) {
            return iterator(p, itb);
          }

          p = np.ptr_;

          if (size_ + 1 > max_load_) {
            this->reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }

          buckets_.insert_node(itb, p);
          ++size_;
          np.ptr_ = node_pointer();
          return iterator(p, itb);
        }

        template <typename Types2>
        void merge_unique(boost::unordered::detail::table<Types2>& other)
        {
          typedef boost::unordered::detail::table<Types2> other_table;
          BOOST_UNORDERED_STATIC_ASSERT(
            (std::is_same<node_type, typename other_table::node_type>::value));
          BOOST_ASSERT(this->node_alloc() == other.node_alloc());

          if (other.size_ == 0) {
            return;
          }

          this->reserve_for_insert(size_ + other.size_);

          iterator last = other.end();
          for (iterator pos = other.begin(); pos != last;) {
            const_key_type& key = other.get_key(pos.p);
            std::size_t const key_hash = this->hash(key);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

            if (this->find_node_impl(key, itb)) {
              ++pos;
              continue;
            }

            iterator old = pos;
            ++pos;

            node_pointer p = other.extract_by_iterator_unique(old);
            buckets_.insert_node(itb, p);
            ++size_;
          }
        }

        ////////////////////////////////////////////////////////////////////////
        // Insert range methods
        //
        // if hash function throws, or inserting > 1 element, basic exception
        // safety strong otherwise

        template <class InputIt>
        void insert_range_unique(no_key, InputIt i, InputIt j)
        {
          hasher const& hf = this->hash_function();
          node_allocator_type alloc = this->node_alloc();

          for (; i != j; ++i) {
            node_tmp tmp(detail::func::construct_node(alloc, *i), alloc);

            value_type const& value = tmp.node_->value();
            const_key_type& key = extractor::extract(value);
            std::size_t const h = hf(key);

            bucket_iterator itb = buckets_.at(buckets_.position(h));
            node_pointer it = find_node_impl(key, itb);
            if (it) {
              continue;
            }

            if (size_ + 1 > max_load_) {
              reserve(size_ + 1);
              itb = buckets_.at(buckets_.position(h));
            }

            node_pointer nptr = tmp.release();
            buckets_.insert_node(itb, nptr);
            ++size_;
          }
        }

        ////////////////////////////////////////////////////////////////////////
        // Extract

        inline node_pointer extract_by_iterator_unique(c_iterator i)
        {
          node_pointer p = i.p;
          bucket_iterator itb = i.itb;

          buckets_.extract_node(itb, p);
          --size_;

          return p;
        }

        ////////////////////////////////////////////////////////////////////////
        // Erase
        //

        template <class Key> std::size_t erase_key_unique_impl(Key const& k)
        {
          bucket_iterator itb = buckets_.at(buckets_.position(this->hash(k)));
          node_pointer* pp = this->find_prev(k, itb);
          if (!pp) {
            return 0;
          }

          node_pointer p = *pp;
          buckets_.extract_node_after(itb, pp);
          this->delete_node(p);
          --size_;
          return 1;
        }

        iterator erase_node(c_iterator pos)
        {
          c_iterator next = pos;
          ++next;

          bucket_iterator itb = pos.itb;
          node_pointer* pp = std::addressof(itb->next);
          while (*pp != pos.p) {
            pp = std::addressof((*pp)->next);
          }

          buckets_.extract_node_after(itb, pp);
          this->delete_node(pos.p);
          --size_;

          return iterator(next.p, next.itb);
        }

        iterator erase_nodes_range(c_iterator first, c_iterator last)
        {
          if (first == last) {
            return iterator(last.p, last.itb);
          }

          // though `first` stores of a copy of a pointer to a node, we wish to
          // mutate the pointers stored internally by the singly-linked list in
          // each bucket group so we have to retrieve it manually by iterating
          //
          bucket_iterator itb = first.itb;
          node_pointer* pp = std::addressof(itb->next);
          while (*pp != first.p) {
            pp = std::addressof((*pp)->next);
          }

          while (*pp != last.p) {
            node_pointer p = *pp;
            *pp = (*pp)->next;

            this->delete_node(p);
            --size_;

            bool const at_end = !(*pp);
            bool const is_empty_bucket = !itb->next;

            if (at_end) {
              if (is_empty_bucket) {
                buckets_.unlink_bucket(itb++);
              } else {
                ++itb;
              }
              pp = std::addressof(itb->next);
            }
          }

          return iterator(last.p, last.itb);
        }

        ////////////////////////////////////////////////////////////////////////
        // fill_buckets_unique

        void copy_buckets(table const& src, std::true_type)
        {
          BOOST_ASSERT(size_ == 0);

          this->reserve_for_insert(src.size_);

          for (iterator pos = src.begin(); pos != src.end(); ++pos) {
            value_type const& value = *pos;
            const_key_type& key = extractor::extract(value);
            std::size_t const key_hash = this->hash(key);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

            node_allocator_type alloc = this->node_alloc();
            node_tmp tmp(detail::func::construct_node(alloc, value), alloc);

            buckets_.insert_node(itb, tmp.release());
            ++size_;
          }
        }

        void move_assign_buckets(table& src, std::true_type)
        {
          BOOST_ASSERT(size_ == 0);
          BOOST_ASSERT(max_load_ >= src.size_);

          iterator last = src.end();
          node_allocator_type alloc = this->node_alloc();

          for (iterator pos = src.begin(); pos != last; ++pos) {
            value_type value = std::move(*pos);
            const_key_type& key = extractor::extract(value);
            std::size_t const key_hash = this->hash(key);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

            node_tmp tmp(
              detail::func::construct_node(alloc, std::move(value)), alloc);

            buckets_.insert_node(itb, tmp.release());
            ++size_;
          }
        }

        ////////////////////////////////////////////////////////////////////////
        // Equivalent keys

        // Equality

        bool equals_equiv(table const& other) const
        {
          if (this->size_ != other.size_)
            return false;

          iterator last = this->end();
          for (iterator n1 = this->begin(); n1 != last;) {
            const_key_type& k = extractor::extract(*n1);
            iterator n2 = other.find(k);
            if (n2 == other.end()) {
              return false;
            }

            iterator end1 = this->next_group(k, n1);
            iterator end2 = other.next_group(k, n2);

            if (!group_equals_equiv(n1, end1, n2, end2)) {
              return false;
            }

            n1 = end1;
          }

          return true;
        }

        static bool group_equals_equiv(
          iterator n1, iterator end1, iterator n2, iterator end2)
        {
          for (;;) {
            if (*n1 != *n2)
              break;

            ++n1;
            ++n2;

            if (n1 == end1)
              return n2 == end2;

            if (n2 == end2)
              return false;
          }

          for (iterator n1a = n1, n2a = n2;;) {
            ++n1a;
            ++n2a;

            if (n1a == end1) {
              if (n2a == end2)
                break;
              else
                return false;
            }

            if (n2a == end2)
              return false;
          }

          iterator start = n1;
          for (; n1 != end1; ++n1) {
            value_type const& v = *n1;
            if (!find_equiv(start, n1, v)) {
              std::size_t matches = count_equal_equiv(n2, end2, v);
              if (!matches)
                return false;

              iterator t = n1;
              if (matches != 1 + count_equal_equiv(++t, end1, v))
                return false;
            }
          }

          return true;
        }

        static bool find_equiv(iterator n, iterator last, value_type const& v)
        {
          for (; n != last; ++n)
            if (*n == v)
              return true;
          return false;
        }

        static std::size_t count_equal_equiv(
          iterator n, iterator last, value_type const& v)
        {
          std::size_t count = 0;
          for (; n != last; ++n)
            if (*n == v)
              ++count;
          return count;
        }

        // Emplace/Insert

        iterator emplace_equiv(node_pointer n)
        {
          node_tmp a(n, this->node_alloc());
          const_key_type& k = this->get_key(a.node_);
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer hint = this->find_node_impl(k, itb);

          if (size_ + 1 > max_load_) {
            this->reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }
          node_pointer p = a.release();
          buckets_.insert_node_hint(itb, p, hint);
          ++size_;
          return iterator(p, itb);
        }

        iterator emplace_hint_equiv(c_iterator hint, node_pointer n)
        {
          node_tmp a(n, this->node_alloc());
          const_key_type& k = this->get_key(a.node_);
          bucket_iterator itb = hint.itb;
          node_pointer p = hint.p;

          std::size_t key_hash = 0u;

          bool const needs_rehash = (size_ + 1 > max_load_);
          bool const usable_hint = (p && this->key_eq()(k, this->get_key(p)));

          if (!usable_hint) {
            key_hash = this->hash(k);
            itb = buckets_.at(buckets_.position(key_hash));
            p = this->find_node_impl(k, itb);
          } else if (usable_hint && needs_rehash) {
            key_hash = this->hash(k);
          }

          if (needs_rehash) {
            this->reserve(size_ + 1);
            itb = buckets_.at(buckets_.position(key_hash));
          }

          a.release();
          buckets_.insert_node_hint(itb, n, p);
          ++size_;
          return iterator(n, itb);
        }

        void emplace_no_rehash_equiv(node_pointer n)
        {
          BOOST_ASSERT(size_ + 1 <= max_load_);
          node_tmp a(n, this->node_alloc());
          const_key_type& k = this->get_key(a.node_);
          std::size_t key_hash = this->hash(k);
          bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
          node_pointer hint = this->find_node_impl(k, itb);
          node_pointer p = a.release();
          buckets_.insert_node_hint(itb, p, hint);
          ++size_;
        }

        template <typename NodeType>
        iterator move_insert_node_type_equiv(NodeType& np)
        {
          iterator result;

          if (np) {
            this->reserve_for_insert(size_ + 1);

            const_key_type& k = this->get_key(np.ptr_);
            std::size_t key_hash = this->hash(k);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

            node_pointer hint = this->find_node_impl(k, itb);
            buckets_.insert_node_hint(itb, np.ptr_, hint);
            ++size_;

            result = iterator(np.ptr_, itb);
            np.ptr_ = node_pointer();
          }

          return result;
        }

        template <typename NodeType>
        iterator move_insert_node_type_with_hint_equiv(
          c_iterator hint, NodeType& np)
        {
          iterator result;
          if (np) {
            bucket_iterator itb = hint.itb;
            node_pointer pos = hint.p;
            const_key_type& k = this->get_key(np.ptr_);
            std::size_t key_hash = this->hash(k);
            if (size_ + 1 > max_load_) {
              this->reserve(size_ + 1);
              itb = buckets_.at(buckets_.position(key_hash));
            }

            if (hint.p && this->key_eq()(k, this->get_key(hint.p))) {
            } else {
              itb = buckets_.at(buckets_.position(key_hash));
              pos = this->find_node_impl(k, itb);
            }
            buckets_.insert_node_hint(itb, np.ptr_, pos);
            ++size_;
            result = iterator(np.ptr_, itb);

            np.ptr_ = node_pointer();
          }

          return result;
        }

        ////////////////////////////////////////////////////////////////////////
        // Insert range methods

        // if hash function throws, or inserting > 1 element, basic exception
        // safety. Strong otherwise
        template <class I>
        typename boost::unordered::detail::enable_if_forward<I, void>::type
        insert_range_equiv(I i, I j)
        {
          if (i == j)
            return;

          std::size_t distance = static_cast<std::size_t>(std::distance(i, j));
          if (distance == 1) {
            emplace_equiv(boost::unordered::detail::func::construct_node(
              this->node_alloc(), *i));
          } else {
            // Only require basic exception safety here
            this->reserve_for_insert(size_ + distance);

            for (; i != j; ++i) {
              emplace_no_rehash_equiv(
                boost::unordered::detail::func::construct_node(
                  this->node_alloc(), *i));
            }
          }
        }

        template <class I>
        typename boost::unordered::detail::disable_if_forward<I, void>::type
        insert_range_equiv(I i, I j)
        {
          for (; i != j; ++i) {
            emplace_equiv(boost::unordered::detail::func::construct_node(
              this->node_alloc(), *i));
          }
        }

        ////////////////////////////////////////////////////////////////////////
        // Extract

        inline node_pointer extract_by_iterator_equiv(c_iterator n)
        {
          node_pointer p = n.p;
          bucket_iterator itb = n.itb;
          buckets_.extract_node(itb, p);
          --size_;
          return p;
        }

        ////////////////////////////////////////////////////////////////////////
        // Erase
        //
        // no throw

        template <class Key> std::size_t erase_key_equiv_impl(Key const& k)
        {
          std::size_t deleted_count = 0;

          bucket_iterator itb = buckets_.at(buckets_.position(this->hash(k)));
          node_pointer* pp = this->find_prev(k, itb);
          if (pp) {
            while (*pp && this->key_eq()(this->get_key(*pp), k)) {
              node_pointer p = *pp;
              *pp = (*pp)->next;

              this->delete_node(p);
              --size_;
              ++deleted_count;
            }

            if (!itb->next) {
              buckets_.unlink_bucket(itb);
            }
          }
          return deleted_count;
        }

        std::size_t erase_key_equiv(const_key_type& k)
        {
          return this->erase_key_equiv_impl(k);
        }

        ////////////////////////////////////////////////////////////////////////
        // fill_buckets

        void copy_buckets(table const& src, std::false_type)
        {
          BOOST_ASSERT(size_ == 0);

          this->reserve_for_insert(src.size_);

          iterator last = src.end();
          for (iterator pos = src.begin(); pos != last; ++pos) {
            value_type const& value = *pos;
            const_key_type& key = extractor::extract(value);
            std::size_t const key_hash = this->hash(key);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));
            node_allocator_type alloc = this->node_alloc();
            node_tmp tmp(detail::func::construct_node(alloc, value), alloc);
            node_pointer hint = this->find_node_impl(key, itb);
            buckets_.insert_node_hint(itb, tmp.release(), hint);
            ++size_;
          }
        }

        void move_assign_buckets(table& src, std::false_type)
        {
          BOOST_ASSERT(size_ == 0);
          BOOST_ASSERT(max_load_ >= src.size_);

          iterator last = src.end();
          node_allocator_type alloc = this->node_alloc();

          for (iterator pos = src.begin(); pos != last; ++pos) {
            value_type value = std::move(*pos);
            const_key_type& key = extractor::extract(value);
            std::size_t const key_hash = this->hash(key);

            bucket_iterator itb = buckets_.at(buckets_.position(key_hash));

            node_pointer hint = this->find_node_impl(key, itb);
            node_tmp tmp(
              detail::func::construct_node(alloc, std::move(value)), alloc);

            buckets_.insert_node_hint(itb, tmp.release(), hint);
            ++size_;
          }
        }
      };

      //////////////////////////////////////////////////////////////////////////
      // Clear

      template <typename Types> inline void table<Types>::clear_impl()
      {
        bucket_iterator itb = buckets_.begin(), last = buckets_.end();
        for (; itb != last;) {
          bucket_iterator next_itb = itb;
          ++next_itb;
          node_pointer* pp = std::addressof(itb->next);
          while (*pp) {
            node_pointer p = *pp;
            buckets_.extract_node_after(itb, pp);
            this->delete_node(p);
            --size_;
          }
          itb = next_itb;
        }
      }

      //////////////////////////////////////////////////////////////////////////
      // Reserve & Rehash

      // if hash function throws, basic exception safety
      // strong otherwise.
      template <typename Types>
      inline void table<Types>::rehash(std::size_t num_buckets)
      {
        num_buckets = buckets_.bucket_count_for(
          (std::max)(min_buckets(size_, mlf_), num_buckets));

        if (num_buckets != this->bucket_count()) {
          this->rehash_impl(num_buckets);
        }
      }

      template <class Types>
      inline void table<Types>::reserve(std::size_t num_elements)
      {
        std::size_t num_buckets = min_buckets(num_elements, mlf_);
        this->rehash(num_buckets);
      }

      template <class Types>
      inline void table<Types>::reserve_for_insert(std::size_t num_elements)
      {
        if (num_elements > max_load_) {
          std::size_t const num_buckets = static_cast<std::size_t>(
            1.0f + std::ceil(static_cast<float>(num_elements) / mlf_));

          this->rehash_impl(num_buckets);
        }
      }

      template <class Types>
      inline void table<Types>::rehash_impl(std::size_t num_buckets)
      {
        bucket_array_type new_buckets(
          num_buckets, buckets_.get_allocator());

        BOOST_TRY
        {
          boost::unordered::detail::span<bucket_type> bspan = buckets_.raw();

          bucket_type* pos = bspan.data;
          std::size_t size = bspan.size;
          bucket_type* last = pos + size;

          for (; pos != last; ++pos) {
            bucket_type& b = *pos;
            for (node_pointer p = b.next; p;) {
              node_pointer next_p = p->next;
              transfer_node(p, b, new_buckets);
              p = next_p;
              b.next = p;
            }
          }
        }
        BOOST_CATCH(...)
        {
          for (bucket_iterator pos = new_buckets.begin();
               pos != new_buckets.end(); ++pos) {
            bucket_type& b = *pos;
            for (node_pointer p = b.next; p;) {
              node_pointer next_p = p->next;
              delete_node(p);
              --size_;
              p = next_p;
            }
          }
          buckets_.unlink_empty_buckets();
          BOOST_RETHROW
        }
        BOOST_CATCH_END

        buckets_ = std::move(new_buckets);
        recalculate_max_load();
      }

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

      ////////////////////////////////////////////////////////////////////////
      // key extractors
      //
      // no throw
      //
      // 'extract_key' is called with the emplace parameters to return a
      // key if available or 'no_key' is one isn't and will need to be
      // constructed. This could be done by overloading the emplace
      // implementation
      // for the different cases, but that's a bit tricky on compilers without
      // variadic templates.

      template <typename Key, typename T> struct is_key
      {
        template <typename T2> static choice1::type test(T2 const&);
        static choice2::type test(Key const&);

        enum
        {
          value = sizeof(test(boost::unordered::detail::make<T>())) ==
                  sizeof(choice2::type)
        };

        typedef typename std::conditional<value, Key const&, no_key>::type type;
      };

      template <class ValueType> struct set_extractor
      {
        typedef ValueType value_type;
        typedef ValueType key_type;

        static key_type const& extract(value_type const& v) { return v; }

        static key_type const& extract(value_type&& v) { return v; }

        static no_key extract() { return no_key(); }

        template <class Arg> static no_key extract(Arg const&)
        {
          return no_key();
        }

        template <class Arg1, class Arg2, class... Args>
        static no_key extract(Arg1 const&, Arg2 const&, Args const&...)
        {
          return no_key();
        }
      };

      template <class ValueType> struct map_extractor
      {
        typedef ValueType value_type;
        typedef typename std::remove_const<typename boost::unordered::detail::
            pair_traits<ValueType>::first_type>::type key_type;

        static key_type const& extract(value_type const& v) { return v.first; }

        template <class Second>
        static key_type const& extract(std::pair<key_type, Second> const& v)
        {
          return v.first;
        }

        template <class Second>
        static key_type const& extract(
          std::pair<key_type const, Second> const& v)
        {
          return v.first;
        }

        template <class Arg1>
        static key_type const& extract(key_type const& k, Arg1 const&)
        {
          return k;
        }

        static no_key extract() { return no_key(); }

        template <class Arg> static no_key extract(Arg const&)
        {
          return no_key();
        }

        template <class Arg1, class Arg2>
        static typename std::conditional<
          (is_similar<Arg1, key_type>::value ||
            is_complete_and_move_constructible<key_type>::value),
          converting_key, no_key>::type
        extract(Arg1 const&, Arg2 const&)
        {
          return {};
        }

        template <class Arg1, class Arg2, class Arg3, class... Args>
        static no_key extract(
          Arg1 const&, Arg2 const&, Arg3 const&, Args const&...)
        {
          return no_key();
        }

        template <template <class...> class Tuple, typename T2>
        static no_key extract(
          std::piecewise_construct_t, Tuple<> const&, T2 const&)
        {
          return no_key();
        }

        template <template <typename...> class Tuple, typename T, typename T2,
          typename... Args>
        static auto extract(
          std::piecewise_construct_t, Tuple<T, Args...> const& k, T2 const&) ->
          typename std::enable_if<
            !std::is_same<T, boost::tuples::null_type>::value,
            typename is_key<key_type, T>::type>::type
        {
          using std::get;
          return typename is_key<key_type, T>::type(get<0>(k));
        }
      };

      template <class Container, class Predicate>
      typename Container::size_type erase_if(Container& c, Predicate& pred)
      {
        typedef typename Container::size_type size_type;
        typedef typename Container::iterator iterator;

        size_type const size = c.size();

        for (iterator pos = c.begin(), last = c.end(); pos != last;) {
          if (pred(*pos)) {
            pos = c.erase(pos);
          } else {
            ++pos;
          }
        }

        return (size - c.size());
      }
    } // namespace detail
  } // namespace unordered
} // namespace boost

#endif
