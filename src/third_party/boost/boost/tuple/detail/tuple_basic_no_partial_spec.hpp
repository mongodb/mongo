// - tuple_basic_no_partial_spec.hpp -----------------------------------------

// Copyright (C) 1999, 2000 Jaakko Jarvi (jaakko.jarvi@cs.utu.fi)
// Copyright (C) 2001 Douglas Gregor (gregod@rpi.edu)
// Copyright (C) 2001 Gary Powell (gary.powell@sierra.com)
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org or http://lambda.cs.utu.fi

// Revision History
//  14 02 01    Remove extra ';'. Also, fixed 10-parameter to make_tuple. (DG)
//  10 02 01    Fixed "null_type" constructors.
//              Implemented comparison operators globally.
//              Hide element_type_ref and element_type_const_ref.
//              (DG).
//  09 02 01    Extended to tuples of length 10. Changed comparison for
//              operator<()
//              to the same used by std::pair<>, added cnull_type() (GP)
//  03 02 01    Initial Version from original tuple.hpp code by JJ. (DG)

// -----------------------------------------------------------------

#ifndef BOOST_TUPLE_BASIC_NO_PARTIAL_SPEC_HPP
#define BOOST_TUPLE_BASIC_NO_PARTIAL_SPEC_HPP

#include "boost/type_traits.hpp"
#include "boost/utility/swap.hpp"
#include <utility>

#if defined BOOST_MSVC
#pragma warning(disable:4518) // storage-class or type specifier(s) unexpected here; ignored
#pragma warning(disable:4181) // qualifier applied to reference type ignored
#pragma warning(disable:4227) // qualifier applied to reference type ignored
#endif

namespace boost {
namespace tuples {

    // null_type denotes the end of a list built with "cons"
    struct null_type
    {
      null_type() {}
      null_type(const null_type&, const null_type&) {}
    };

    // a helper function to provide a const null_type type temporary
    inline const null_type cnull_type() { return null_type(); }

// forward declaration of tuple
    template<
      typename T1 = null_type,
      typename T2 = null_type,
      typename T3 = null_type,
      typename T4 = null_type,
      typename T5 = null_type,
      typename T6 = null_type,
      typename T7 = null_type,
      typename T8 = null_type,
      typename T9 = null_type,
      typename T10 = null_type
    >
    class tuple;

// forward declaration of cons
    template<typename Head, typename Tail = null_type>
    struct cons;

    namespace detail {

      // Takes a pointer and routes all assignments to whatever it points to
      template<typename T>
      struct assign_to_pointee
      {
      public:
        explicit assign_to_pointee(T* p) : ptr(p) {}

        template<typename Other>
        assign_to_pointee& operator=(const Other& other)
        {
          *ptr = other;
          return *this;
        }

      private:
        T* ptr;
      };

      // Swallows any assignment
      struct swallow_assign
      {
        template<typename T>
        swallow_assign const& operator=(const T&) const
        {
          return *this;
        }
      };

    template <typename T> struct add_const_reference : add_reference<typename add_const<T>::type> {};

    template <class MyTail>
    struct init_tail
    {
        // Each of vc6 and vc7 seem to require a different formulation
        // of this return type
        template <class H, class T>
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
        static typename add_reference<typename add_const<T>::type>::type
#else
        static typename add_const_reference<T>::type
#endif
        execute( cons<H,T> const& u, long )
        {
            return u.get_tail();
        }
    };

    template <>
    struct init_tail<null_type>
    {
        template <class H>
        static null_type execute( cons<H,null_type> const& u, long )
        {
            return null_type();
        }

        template <class U>
        static null_type execute(U const&, ...)
        {
            return null_type();
        }
     private:
        template <class H, class T>
        void execute( cons<H,T> const&, int);
    };

    template <class Other>
    Other const&
    init_head( Other const& u, ... )
    {
        return u;
    }

    template <class H, class T>
    typename add_reference<typename add_const<H>::type>::type
    init_head( cons<H,T> const& u, int )
    {
        return u.get_head();
    }

    inline char**** init_head(null_type const&, int);

  } // end of namespace detail

    // cons builds a heterogenous list of types
   template<typename Head, typename Tail>
   struct cons
   {
     typedef cons self_type;
     typedef Head head_type;
     typedef Tail tail_type;

    private:
       typedef typename boost::add_reference<head_type>::type head_ref;
       typedef typename boost::add_reference<tail_type>::type tail_ref;
       typedef typename detail::add_const_reference<head_type>::type head_cref;
       typedef typename detail::add_const_reference<tail_type>::type tail_cref;
    public:
     head_type head;
     tail_type tail;

     head_ref get_head() { return head; }
     tail_ref get_tail() { return tail; }

     head_cref get_head() const { return head; }
     tail_cref get_tail() const { return tail; }

     cons() : head(), tail() {}

#if defined BOOST_MSVC
      template<typename Tail>
      cons(head_cref h /* = head_type() */, // causes MSVC 6.5 to barf.
                    const Tail& t) : head(h), tail(t.head, t.tail)
      {
      }

      cons(head_cref h /* = head_type() */, // causes MSVC 6.5 to barf.
                    const null_type& t) : head(h), tail(t)
      {
      }

#else
      template<typename T>
      explicit cons(head_cref h, const T& t) :
        head(h), tail(t.head, t.tail)
      {
      }

      explicit cons(head_cref h = head_type(),
                    tail_cref t = tail_type()) :
        head(h), tail(t)
      {
      }
#endif

      template <class U>
      cons( const U& u )
        : head(detail::init_head(u, 0))
        , tail(detail::init_tail<Tail>::execute(u, 0L))
       {
       }

      template<typename Other>
      cons& operator=(const Other& other)
      {
        head = other.head;
        tail = other.tail;
        return *this;
      }
    };

    namespace detail {

      // Determines if the parameter is null_type
      template<typename T> struct is_null_type { enum { RET = 0 }; };
      template<> struct is_null_type<null_type> { enum { RET = 1 }; };

      /* Build a cons structure from the given Head and Tail. If both are null_type,
      return null_type. */
      template<typename Head, typename Tail>
      struct build_cons
      {
      private:
        enum { tail_is_null_type = is_null_type<Tail>::RET };
      public:
        typedef cons<Head, Tail> RET;
      };

      template<>
      struct build_cons<null_type, null_type>
      {
        typedef null_type RET;
      };

      // Map the N elements of a tuple into a cons list
      template<
        typename T1,
        typename T2 = null_type,
        typename T3 = null_type,
        typename T4 = null_type,
        typename T5 = null_type,
        typename T6 = null_type,
        typename T7 = null_type,
        typename T8 = null_type,
        typename T9 = null_type,
        typename T10 = null_type
      >
      struct map_tuple_to_cons
      {
        typedef typename detail::build_cons<T10, null_type  >::RET cons10;
        typedef typename detail::build_cons<T9, cons10>::RET cons9;
        typedef typename detail::build_cons<T8, cons9>::RET cons8;
        typedef typename detail::build_cons<T7, cons8>::RET cons7;
        typedef typename detail::build_cons<T6, cons7>::RET cons6;
        typedef typename detail::build_cons<T5, cons6>::RET cons5;
        typedef typename detail::build_cons<T4, cons5>::RET cons4;
        typedef typename detail::build_cons<T3, cons4>::RET cons3;
        typedef typename detail::build_cons<T2, cons3>::RET cons2;
        typedef typename detail::build_cons<T1, cons2>::RET cons1;
      };

      // Workaround the lack of partial specialization in some compilers
      template<int N>
      struct _element_type
      {
        template<typename Tuple>
        struct inner
        {
        private:
          typedef typename Tuple::tail_type tail_type;
          typedef _element_type<N-1> next_elt_type;

        public:
          typedef typename _element_type<N-1>::template inner<tail_type>::RET RET;
        };
      };

      template<>
      struct _element_type<0>
      {
        template<typename Tuple>
        struct inner
        {
          typedef typename Tuple::head_type RET;
        };
      };

    } // namespace detail


    // Return the Nth type of the given Tuple
    template<int N, typename Tuple>
    struct element
    {
    private:
      typedef detail::_element_type<N> nth_type;

    public:
      typedef typename nth_type::template inner<Tuple>::RET RET;
      typedef RET type;
    };

    namespace detail {

#if defined(BOOST_MSVC) && (BOOST_MSVC == 1300)
      // special workaround for vc7:

      template <bool x>
      struct reference_adder
      {
         template <class T>
         struct rebind
         {
            typedef T& type;
         };
      };

      template <>
      struct reference_adder<true>
      {
         template <class T>
         struct rebind
         {
            typedef T type;
         };
      };


      // Return a reference to the Nth type of the given Tuple
      template<int N, typename Tuple>
      struct element_ref
      {
      private:
         typedef typename element<N, Tuple>::RET elt_type;
         enum { is_ref = is_reference<elt_type>::value };

      public:
         typedef reference_adder<is_ref>::rebind<elt_type>::type RET;
         typedef RET type;
      };

      // Return a const reference to the Nth type of the given Tuple
      template<int N, typename Tuple>
      struct element_const_ref
      {
      private:
         typedef typename element<N, Tuple>::RET elt_type;
         enum { is_ref = is_reference<elt_type>::value };

      public:
         typedef reference_adder<is_ref>::rebind<const elt_type>::type RET;
         typedef RET type;
      };

#else // vc7

      // Return a reference to the Nth type of the given Tuple
      template<int N, typename Tuple>
      struct element_ref
      {
      private:
        typedef typename element<N, Tuple>::RET elt_type;

      public:
        typedef typename add_reference<elt_type>::type RET;
        typedef RET type;
      };

      // Return a const reference to the Nth type of the given Tuple
      template<int N, typename Tuple>
      struct element_const_ref
      {
      private:
        typedef typename element<N, Tuple>::RET elt_type;

      public:
        typedef typename add_reference<const elt_type>::type RET;
        typedef RET type;
      };
#endif // vc7

    } // namespace detail

    // Get length of this tuple
    template<typename Tuple>
    struct length
    {
      BOOST_STATIC_CONSTANT(int, value = 1 + length<typename Tuple::tail_type>::value);
    };

    template<> struct length<tuple<> > {
      BOOST_STATIC_CONSTANT(int, value = 0);
    };

    template<>
    struct length<null_type>
    {
      BOOST_STATIC_CONSTANT(int, value = 0);
    };

    namespace detail {

    // Reference the Nth element in a tuple and retrieve it with "get"
    template<int N>
    struct get_class
    {
      template<typename Head, typename Tail>
      static inline
      typename detail::element_ref<N, cons<Head, Tail> >::RET
      get(cons<Head, Tail>& t)
      {
        return get_class<N-1>::get(t.tail);
      }

      template<typename Head, typename Tail>
      static inline
      typename detail::element_const_ref<N, cons<Head, Tail> >::RET
      get(const cons<Head, Tail>& t)
      {
        return get_class<N-1>::get(t.tail);
      }
    };

    template<>
    struct get_class<0>
    {
      template<typename Head, typename Tail>
      static inline
      typename add_reference<Head>::type
      get(cons<Head, Tail>& t)
      {
        return t.head;
      }

      template<typename Head, typename Tail>
      static inline
      typename add_reference<const Head>::type
      get(const cons<Head, Tail>& t)
      {
        return t.head;
      }
    };

    } // namespace detail

    // tuple class
    template<
      typename T1,
      typename T2,
      typename T3,
      typename T4,
      typename T5,
      typename T6,
      typename T7,
      typename T8,
      typename T9,
      typename T10
    >
    class tuple :
      public detail::map_tuple_to_cons<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10>::cons1
    {
    private:
      typedef detail::map_tuple_to_cons<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10> mapped_tuple;
      typedef typename mapped_tuple::cons10 cons10;
      typedef typename mapped_tuple::cons9 cons9;
      typedef typename mapped_tuple::cons8 cons8;
      typedef typename mapped_tuple::cons7 cons7;
      typedef typename mapped_tuple::cons6 cons6;
      typedef typename mapped_tuple::cons5 cons5;
      typedef typename mapped_tuple::cons4 cons4;
      typedef typename mapped_tuple::cons3 cons3;
      typedef typename mapped_tuple::cons2 cons2;
      typedef typename mapped_tuple::cons1 cons1;

      typedef typename detail::add_const_reference<T1>::type t1_cref;
      typedef typename detail::add_const_reference<T2>::type t2_cref;
      typedef typename detail::add_const_reference<T3>::type t3_cref;
      typedef typename detail::add_const_reference<T4>::type t4_cref;
      typedef typename detail::add_const_reference<T5>::type t5_cref;
      typedef typename detail::add_const_reference<T6>::type t6_cref;
      typedef typename detail::add_const_reference<T7>::type t7_cref;
      typedef typename detail::add_const_reference<T8>::type t8_cref;
      typedef typename detail::add_const_reference<T9>::type t9_cref;
      typedef typename detail::add_const_reference<T10>::type t10_cref;
    public:
      typedef cons1 inherited;
      typedef tuple self_type;

      tuple() : cons1(T1(), cons2(T2(), cons3(T3(), cons4(T4(), cons5(T5(), cons6(T6(),cons7(T7(),cons8(T8(),cons9(T9(),cons10(T10()))))))))))
        {}

      tuple(
          t1_cref t1,
          t2_cref t2,
          t3_cref t3 = T3(),
          t4_cref t4 = T4(),
          t5_cref t5 = T5(),
          t6_cref t6 = T6(),
          t7_cref t7 = T7(),
          t8_cref t8 = T8(),
          t9_cref t9 = T9(),
          t10_cref t10 = T10()
      ) :
        cons1(t1, cons2(t2, cons3(t3, cons4(t4, cons5(t5, cons6(t6,cons7(t7,cons8(t8,cons9(t9,cons10(t10))))))))))
      {
      }

      explicit tuple(t1_cref t1)
        : cons1(t1, cons2(T2(), cons3(T3(), cons4(T4(), cons5(T5(), cons6(T6(),cons7(T7(),cons8(T8(),cons9(T9(),cons10(T10()))))))))))
      {}

      template<typename Head, typename Tail>
      tuple(const cons<Head, Tail>& other) :
        cons1(other.head, other.tail)
      {
      }

      template<typename First, typename Second>
      self_type& operator=(const std::pair<First, Second>& other)
      {
        this->head = other.first;
        this->tail.head = other.second;
        return *this;
      }

      template<typename Head, typename Tail>
      self_type& operator=(const cons<Head, Tail>& other)
      {
        this->head = other.head;
        this->tail = other.tail;

        return *this;
      }
    };

    namespace detail {

      template<int N> struct workaround_holder {};

    } // namespace detail

    template<int N, typename Head, typename Tail>
    typename detail::element_ref<N, cons<Head, Tail> >::RET
    get(cons<Head, Tail>& t, detail::workaround_holder<N>* = 0)
    {
      return detail::get_class<N>::get(t);
    }

    template<int N, typename Head, typename Tail>
    typename detail::element_const_ref<N, cons<Head, Tail> >::RET
    get(const cons<Head, Tail>& t, detail::workaround_holder<N>* = 0)
    {
      return detail::get_class<N>::get(t);
    }

    // Make a tuple
    template<typename T1>
    inline
    tuple<T1>
    make_tuple(const T1& t1)
    {
      return tuple<T1>(t1);
    }

    // Make a tuple
    template<typename T1, typename T2>
    inline
    tuple<T1, T2>
    make_tuple(const T1& t1, const T2& t2)
    {
      return tuple<T1, T2>(t1, t2);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3>
    inline
    tuple<T1, T2, T3>
    make_tuple(const T1& t1, const T2& t2, const T3& t3)
    {
      return tuple<T1, T2, T3>(t1, t2, t3);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4>
    inline
    tuple<T1, T2, T3, T4>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4)
    {
      return tuple<T1, T2, T3, T4>(t1, t2, t3, t4);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5>
    inline
    tuple<T1, T2, T3, T4, T5>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5)
    {
      return tuple<T1, T2, T3, T4, T5>(t1, t2, t3, t4, t5);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
    inline
    tuple<T1, T2, T3, T4, T5, T6>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5, const T6& t6)
    {
      return tuple<T1, T2, T3, T4, T5, T6>(t1, t2, t3, t4, t5, t6);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
    inline
    tuple<T1, T2, T3, T4, T5, T6, T7>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5, const T6& t6, const T7& t7)
    {
      return tuple<T1, T2, T3, T4, T5, T6, T7>(t1, t2, t3, t4, t5, t6, t7);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8>
    inline
    tuple<T1, T2, T3, T4, T5, T6, T7, T8>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5, const T6& t6, const T7& t7, const T8& t8)
    {
      return tuple<T1, T2, T3, T4, T5, T6, T7, T8>(t1, t2, t3, t4, t5, t6, t7, t8);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9>
    inline
    tuple<T1, T2, T3, T4, T5, T6, T7, T8, T9>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5, const T6& t6, const T7& t7, const T8& t8, const T9& t9)
    {
      return tuple<T1, T2, T3, T4, T5, T6, T7, T8, T9>(t1, t2, t3, t4, t5, t6, t7, t8, t9);
    }

    // Make a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10>
    inline
    tuple<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10>
    make_tuple(const T1& t1, const T2& t2, const T3& t3, const T4& t4, const T5& t5, const T6& t6, const T7& t7, const T8& t8, const T9& t9, const T10& t10)
    {
      return tuple<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10>(t1, t2, t3, t4, t5, t6, t7, t8, t9, t10);
    }

    // Tie variables into a tuple
    template<typename T1>
    inline
    tuple<detail::assign_to_pointee<T1> >
    tie(T1& t1)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2> >
    tie(T1& t1, T2& t2)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3> >
    tie(T1& t1, T2& t2, T3& t3)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5>,
      detail::assign_to_pointee<T6> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5, T6 &t6)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5),
                        detail::assign_to_pointee<T6>(&t6));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5>,
      detail::assign_to_pointee<T6>,
      detail::assign_to_pointee<T7> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5, T6 &t6, T7 &t7)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5),
                        detail::assign_to_pointee<T6>(&t6),
                        detail::assign_to_pointee<T7>(&t7));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5>,
      detail::assign_to_pointee<T6>,
      detail::assign_to_pointee<T7>,
      detail::assign_to_pointee<T8> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5, T6 &t6, T7 &t7, T8 &t8)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5),
                        detail::assign_to_pointee<T6>(&t6),
                        detail::assign_to_pointee<T7>(&t7),
                        detail::assign_to_pointee<T8>(&t8));
    }

    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5>,
      detail::assign_to_pointee<T6>,
      detail::assign_to_pointee<T7>,
      detail::assign_to_pointee<T8>,
      detail::assign_to_pointee<T9> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5, T6 &t6, T7 &t7, T8 &t8, T9 &t9)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5),
                        detail::assign_to_pointee<T6>(&t6),
                        detail::assign_to_pointee<T7>(&t7),
                        detail::assign_to_pointee<T8>(&t8),
                        detail::assign_to_pointee<T9>(&t9));
    }
    // Tie variables into a tuple
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10>
    inline
    tuple<detail::assign_to_pointee<T1>,
      detail::assign_to_pointee<T2>,
      detail::assign_to_pointee<T3>,
      detail::assign_to_pointee<T4>,
      detail::assign_to_pointee<T5>,
      detail::assign_to_pointee<T6>,
      detail::assign_to_pointee<T7>,
      detail::assign_to_pointee<T8>,
      detail::assign_to_pointee<T9>,
      detail::assign_to_pointee<T10> >
    tie(T1& t1, T2& t2, T3& t3, T4& t4, T5 &t5, T6 &t6, T7 &t7, T8 &t8, T9 &t9, T10 &t10)
    {
      return make_tuple(detail::assign_to_pointee<T1>(&t1),
                        detail::assign_to_pointee<T2>(&t2),
                        detail::assign_to_pointee<T3>(&t3),
                        detail::assign_to_pointee<T4>(&t4),
                        detail::assign_to_pointee<T5>(&t5),
                        detail::assign_to_pointee<T6>(&t6),
                        detail::assign_to_pointee<T7>(&t7),
                        detail::assign_to_pointee<T8>(&t8),
                        detail::assign_to_pointee<T9>(&t9),
                        detail::assign_to_pointee<T10>(&t10));
    }
    // "ignore" allows tuple positions to be ignored when using "tie".

detail::swallow_assign const ignore = detail::swallow_assign();

template <class T0, class T1, class T2, class T3, class T4,
          class T5, class T6, class T7, class T8, class T9>
void swap(tuple<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>& lhs,
          tuple<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>& rhs);
inline void swap(null_type&, null_type&) {}
template<class HH>
inline void swap(cons<HH, null_type>& lhs, cons<HH, null_type>& rhs) {
  ::boost::swap(lhs.head, rhs.head);
}
template<class HH, class TT>
inline void swap(cons<HH, TT>& lhs, cons<HH, TT>& rhs) {
  ::boost::swap(lhs.head, rhs.head);
  ::boost::tuples::swap(lhs.tail, rhs.tail);
}
template <class T0, class T1, class T2, class T3, class T4,
          class T5, class T6, class T7, class T8, class T9>
inline void swap(tuple<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>& lhs,
          tuple<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9>& rhs) {
  typedef tuple<T0, T1, T2, T3, T4, T5, T6, T7, T8, T9> tuple_type;
  typedef typename tuple_type::inherited base;
  ::boost::tuples::swap(static_cast<base&>(lhs), static_cast<base&>(rhs));
}

} // namespace tuples
} // namespace boost
#endif // BOOST_TUPLE_BASIC_NO_PARTIAL_SPEC_HPP
