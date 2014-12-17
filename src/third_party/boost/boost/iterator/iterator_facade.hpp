// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_FACADE_23022003THW_HPP
#define BOOST_ITERATOR_FACADE_23022003THW_HPP

#include <boost/iterator.hpp>
#include <boost/iterator/interoperable.hpp>
#include <boost/iterator/iterator_traits.hpp>

#include <boost/iterator/detail/facade_iterator_category.hpp>
#include <boost/iterator/detail/enable_if.hpp>

#include <boost/implicit_cast.hpp>
#include <boost/static_assert.hpp>

#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/add_const.hpp>
#include <boost/type_traits/add_pointer.hpp>
#include <boost/type_traits/remove_const.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_pod.hpp>

#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/or.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/not.hpp>
#include <boost/mpl/always.hpp>
#include <boost/mpl/apply.hpp>
#include <boost/mpl/identity.hpp>

#include <boost/iterator/detail/config_def.hpp> // this goes last

namespace boost
{
  // This forward declaration is required for the friend declaration
  // in iterator_core_access
  template <class I, class V, class TC, class R, class D> class iterator_facade;

  namespace detail
  {
    // A binary metafunction class that always returns bool.  VC6
    // ICEs on mpl::always<bool>, probably because of the default
    // parameters.
    struct always_bool2
    {
        template <class T, class U>
        struct apply
        {
            typedef bool type;
        };
    };

    //
    // enable if for use in operator implementation.
    //
    template <
        class Facade1
      , class Facade2
      , class Return
    >
    struct enable_if_interoperable
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
    {
        typedef typename mpl::if_<
            mpl::or_<
                is_convertible<Facade1, Facade2>
              , is_convertible<Facade2, Facade1>
            >
          , Return
          , int[3]
        >::type type;
    };        
#else
      : ::boost::iterators::enable_if<
           mpl::or_<
               is_convertible<Facade1, Facade2>
             , is_convertible<Facade2, Facade1>
           >
         , Return
        >
    {};
#endif 

    //
    // Generates associated types for an iterator_facade with the
    // given parameters.
    //
    template <
        class ValueParam
      , class CategoryOrTraversal
      , class Reference 
      , class Difference
    >
    struct iterator_facade_types
    {
        typedef typename facade_iterator_category<
            CategoryOrTraversal, ValueParam, Reference
        >::type iterator_category;
        
        typedef typename remove_const<ValueParam>::type value_type;
        
        // Not the real associated pointer type
        typedef typename mpl::eval_if<
            boost::detail::iterator_writability_disabled<ValueParam,Reference>
          , add_pointer<const value_type>
          , add_pointer<value_type>
        >::type pointer;
      
# if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)                          \
    && (BOOST_WORKAROUND(_STLPORT_VERSION, BOOST_TESTED_AT(0x452))              \
        || BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, BOOST_TESTED_AT(310)))     \
    || BOOST_WORKAROUND(BOOST_RWSTD_VER, BOOST_TESTED_AT(0x20101))              \
    || BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, <= 310)

        // To interoperate with some broken library/compiler
        // combinations, user-defined iterators must be derived from
        // std::iterator.  It is possible to implement a standard
        // library for broken compilers without this limitation.
#  define BOOST_ITERATOR_FACADE_NEEDS_ITERATOR_BASE 1

        typedef
           iterator<iterator_category, value_type, Difference, pointer, Reference>
        base;
# endif
    };

    // iterators whose dereference operators reference the same value
    // for all iterators into the same sequence (like many input
    // iterators) need help with their postfix ++: the referenced
    // value must be read and stored away before the increment occurs
    // so that *a++ yields the originally referenced element and not
    // the next one.
    template <class Iterator>
    class postfix_increment_proxy
    {
        typedef typename iterator_value<Iterator>::type value_type;
     public:
        explicit postfix_increment_proxy(Iterator const& x)
          : stored_value(*x)
        {}

        // Returning a mutable reference allows nonsense like
        // (*r++).mutate(), but it imposes fewer assumptions about the
        // behavior of the value_type.  In particular, recall taht
        // (*r).mutate() is legal if operator* returns by value.
        value_type&
        operator*() const
        {
            return this->stored_value;
        }
     private:
        mutable value_type stored_value;
    };
    
    //
    // In general, we can't determine that such an iterator isn't
    // writable -- we also need to store a copy of the old iterator so
    // that it can be written into.
    template <class Iterator>
    class writable_postfix_increment_proxy
    {
        typedef typename iterator_value<Iterator>::type value_type;
     public:
        explicit writable_postfix_increment_proxy(Iterator const& x)
          : stored_value(*x)
          , stored_iterator(x)
        {}

        // Dereferencing must return a proxy so that both *r++ = o and
        // value_type(*r++) can work.  In this case, *r is the same as
        // *r++, and the conversion operator below is used to ensure
        // readability.
        writable_postfix_increment_proxy const&
        operator*() const
        {
            return *this;
        }

        // Provides readability of *r++
        operator value_type&() const
        {
            return stored_value;
        }

        // Provides writability of *r++
        template <class T>
        T const& operator=(T const& x) const
        {
            *this->stored_iterator = x;
            return x;
        }

        // This overload just in case only non-const objects are writable
        template <class T>
        T& operator=(T& x) const
        {
            *this->stored_iterator = x;
            return x;
        }

        // Provides X(r++)
        operator Iterator const&() const
        {
            return stored_iterator;
        }
        
     private:
        mutable value_type stored_value;
        Iterator stored_iterator;
    };

# ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

    template <class Reference, class Value>
    struct is_non_proxy_reference_impl
    {
        static Reference r;
        
        template <class R>
        static typename mpl::if_<
            is_convertible<
                R const volatile*
              , Value const volatile*
            >
          , char[1]
          , char[2]
        >::type& helper(R const&);
        
        BOOST_STATIC_CONSTANT(bool, value = sizeof(helper(r)) == 1);
    };
        
    template <class Reference, class Value>
    struct is_non_proxy_reference
      : mpl::bool_<
            is_non_proxy_reference_impl<Reference, Value>::value
        >
    {};
# else 
    template <class Reference, class Value>
    struct is_non_proxy_reference
      : is_convertible<
            typename remove_reference<Reference>::type
            const volatile*
          , Value const volatile*
        >
    {};
# endif 
        
    // A metafunction to choose the result type of postfix ++
    //
    // Because the C++98 input iterator requirements say that *r++ has
    // type T (value_type), implementations of some standard
    // algorithms like lexicographical_compare may use constructions
    // like:
    //
    //          *r++ < *s++
    //
    // If *r++ returns a proxy (as required if r is writable but not
    // multipass), this sort of expression will fail unless the proxy
    // supports the operator<.  Since there are any number of such
    // operations, we're not going to try to support them.  Therefore,
    // even if r++ returns a proxy, *r++ will only return a proxy if
    // *r also returns a proxy.
    template <class Iterator, class Value, class Reference, class CategoryOrTraversal>
    struct postfix_increment_result
      : mpl::eval_if<
            mpl::and_<
                // A proxy is only needed for readable iterators
                is_convertible<Reference,Value const&>
                
                // No multipass iterator can have values that disappear
                // before positions can be re-visited
              , mpl::not_<
                    is_convertible<
                        typename iterator_category_to_traversal<CategoryOrTraversal>::type
                      , forward_traversal_tag
                    >
                >
            >
          , mpl::if_<
                is_non_proxy_reference<Reference,Value>
              , postfix_increment_proxy<Iterator>
              , writable_postfix_increment_proxy<Iterator>
            >
          , mpl::identity<Iterator>
        >
    {};

    // operator->() needs special support for input iterators to strictly meet the
    // standard's requirements. If *i is not a reference type, we must still
    // produce a lvalue to which a pointer can be formed. We do that by
    // returning an instantiation of this special proxy class template.
    template <class T>
    struct operator_arrow_proxy
    {
        operator_arrow_proxy(T const* px) : m_value(*px) {}
        T* operator->() const { return &m_value; }
        // This function is needed for MWCW and BCC, which won't call operator->
        // again automatically per 13.3.1.2 para 8
        operator T*() const { return &m_value; }
        mutable T m_value;
    };

    // A metafunction that gets the result type for operator->.  Also
    // has a static function make() which builds the result from a
    // Reference
    template <class ValueType, class Reference, class Pointer>
    struct operator_arrow_result
    {
        // CWPro8.3 won't accept "operator_arrow_result::type", and we
        // need that type below, so metafunction forwarding would be a
        // losing proposition here.
        typedef typename mpl::if_<
            is_reference<Reference>
          , Pointer
          , operator_arrow_proxy<ValueType>
        >::type type;

        static type make(Reference x)
        {
            return boost::implicit_cast<type>(&x);
        }
    };

# if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
    // Deal with ETI
    template<>
    struct operator_arrow_result<int, int, int>
    {
        typedef int type;
    };
# endif

    // A proxy return type for operator[], needed to deal with
    // iterators that may invalidate referents upon destruction.
    // Consider the temporary iterator in *(a + n)
    template <class Iterator>
    class operator_brackets_proxy
    {
        // Iterator is actually an iterator_facade, so we do not have to
        // go through iterator_traits to access the traits.
        typedef typename Iterator::reference  reference;
        typedef typename Iterator::value_type value_type;

     public:
        operator_brackets_proxy(Iterator const& iter)
          : m_iter(iter)
        {}

        operator reference() const
        {
            return *m_iter;
        }

        operator_brackets_proxy& operator=(value_type const& val)
        {
            *m_iter = val;
            return *this;
        }

     private:
        Iterator m_iter;
    };

    // A metafunction that determines whether operator[] must return a
    // proxy, or whether it can simply return a copy of the value_type.
    template <class ValueType, class Reference>
    struct use_operator_brackets_proxy
      : mpl::not_<
            mpl::and_<
                // Really we want an is_copy_constructible trait here,
                // but is_POD will have to suffice in the meantime.
                boost::is_POD<ValueType>
              , iterator_writability_disabled<ValueType,Reference>
            >
        >
    {};
        
    template <class Iterator, class Value, class Reference>
    struct operator_brackets_result
    {
        typedef typename mpl::if_<
            use_operator_brackets_proxy<Value,Reference>
          , operator_brackets_proxy<Iterator>
          , Value
        >::type type;
    };

    template <class Iterator>
    operator_brackets_proxy<Iterator> make_operator_brackets_result(Iterator const& iter, mpl::true_)
    {
        return operator_brackets_proxy<Iterator>(iter);
    }

    template <class Iterator>
    typename Iterator::value_type make_operator_brackets_result(Iterator const& iter, mpl::false_)
    {
      return *iter;
    }

    struct choose_difference_type
    {
        template <class I1, class I2>
        struct apply
          :
# ifdef BOOST_NO_ONE_WAY_ITERATOR_INTEROP
          iterator_difference<I1>
# elif BOOST_WORKAROUND(BOOST_MSVC, < 1300)
          mpl::if_<
              is_convertible<I2,I1>
            , typename I1::difference_type
            , typename I2::difference_type
          >
# else 
          mpl::eval_if<
              is_convertible<I2,I1>
            , iterator_difference<I1>
            , iterator_difference<I2>
          >
# endif 
        {};

    };
  } // namespace detail


  // Macros which describe the declarations of binary operators
# ifdef BOOST_NO_STRICT_ITERATOR_INTEROPERABILITY
#  define BOOST_ITERATOR_FACADE_INTEROP_HEAD(prefix, op, result_type)       \
    template <                                                              \
        class Derived1, class V1, class TC1, class Reference1, class Difference1 \
      , class Derived2, class V2, class TC2, class Reference2, class Difference2 \
    >                                                                       \
    prefix typename mpl::apply2<result_type,Derived1,Derived2>::type \
    operator op(                                                            \
        iterator_facade<Derived1, V1, TC1, Reference1, Difference1> const& lhs   \
      , iterator_facade<Derived2, V2, TC2, Reference2, Difference2> const& rhs)
# else 
#  define BOOST_ITERATOR_FACADE_INTEROP_HEAD(prefix, op, result_type)   \
    template <                                                          \
        class Derived1, class V1, class TC1, class Reference1, class Difference1 \
      , class Derived2, class V2, class TC2, class Reference2, class Difference2 \
    >                                                                   \
    prefix typename boost::detail::enable_if_interoperable<             \
        Derived1, Derived2                                              \
      , typename mpl::apply2<result_type,Derived1,Derived2>::type       \
    >::type                                                             \
    operator op(                                                        \
        iterator_facade<Derived1, V1, TC1, Reference1, Difference1> const& lhs   \
      , iterator_facade<Derived2, V2, TC2, Reference2, Difference2> const& rhs)
# endif 

#  define BOOST_ITERATOR_FACADE_PLUS_HEAD(prefix,args)              \
    template <class Derived, class V, class TC, class R, class D>   \
    prefix Derived operator+ args

  //
  // Helper class for granting access to the iterator core interface.
  //
  // The simple core interface is used by iterator_facade. The core
  // interface of a user/library defined iterator type should not be made public
  // so that it does not clutter the public interface. Instead iterator_core_access
  // should be made friend so that iterator_facade can access the core
  // interface through iterator_core_access.
  //
  class iterator_core_access
  {
# if defined(BOOST_NO_MEMBER_TEMPLATE_FRIENDS)                  
      // Tasteless as this may seem, making all members public allows member templates
      // to work in the absence of member template friends.
   public:
# else
      
      template <class I, class V, class TC, class R, class D> friend class iterator_facade;

#  define BOOST_ITERATOR_FACADE_RELATION(op)                                \
      BOOST_ITERATOR_FACADE_INTEROP_HEAD(friend,op, boost::detail::always_bool2);

      BOOST_ITERATOR_FACADE_RELATION(==)
      BOOST_ITERATOR_FACADE_RELATION(!=)

      BOOST_ITERATOR_FACADE_RELATION(<)
      BOOST_ITERATOR_FACADE_RELATION(>)
      BOOST_ITERATOR_FACADE_RELATION(<=)
      BOOST_ITERATOR_FACADE_RELATION(>=)
#  undef BOOST_ITERATOR_FACADE_RELATION

      BOOST_ITERATOR_FACADE_INTEROP_HEAD(
          friend, -, boost::detail::choose_difference_type)
      ;

      BOOST_ITERATOR_FACADE_PLUS_HEAD(
          friend inline
          , (iterator_facade<Derived, V, TC, R, D> const&
           , typename Derived::difference_type)
      )
      ;

      BOOST_ITERATOR_FACADE_PLUS_HEAD(
          friend inline
        , (typename Derived::difference_type
           , iterator_facade<Derived, V, TC, R, D> const&)
      )
      ;

# endif

      template <class Facade>
      static typename Facade::reference dereference(Facade const& f)
      {
          return f.dereference();
      }

      template <class Facade>
      static void increment(Facade& f)
      {
          f.increment();
      }

      template <class Facade>
      static void decrement(Facade& f)
      {
          f.decrement();
      }

      template <class Facade1, class Facade2>
      static bool equal(Facade1 const& f1, Facade2 const& f2, mpl::true_)
      {
          return f1.equal(f2);
      }

      template <class Facade1, class Facade2>
      static bool equal(Facade1 const& f1, Facade2 const& f2, mpl::false_)
      {
          return f2.equal(f1);
      }

      template <class Facade>
      static void advance(Facade& f, typename Facade::difference_type n)
      {
          f.advance(n);
      }

      template <class Facade1, class Facade2>
      static typename Facade1::difference_type distance_from(
          Facade1 const& f1, Facade2 const& f2, mpl::true_)
      {
          return -f1.distance_to(f2);
      }

      template <class Facade1, class Facade2>
      static typename Facade2::difference_type distance_from(
          Facade1 const& f1, Facade2 const& f2, mpl::false_)
      {
          return f2.distance_to(f1);
      }

      //
      // Curiously Recurring Template interface.
      //
      template <class I, class V, class TC, class R, class D>
      static I& derived(iterator_facade<I,V,TC,R,D>& facade)
      {
          return *static_cast<I*>(&facade);
      }

      template <class I, class V, class TC, class R, class D>
      static I const& derived(iterator_facade<I,V,TC,R,D> const& facade)
      {
          return *static_cast<I const*>(&facade);
      }

   private:
      // objects of this class are useless
      iterator_core_access(); //undefined
  };

  //
  // iterator_facade - use as a public base class for defining new
  // standard-conforming iterators.
  //
  template <
      class Derived             // The derived iterator type being constructed
    , class Value
    , class CategoryOrTraversal
    , class Reference   = Value&
    , class Difference  = std::ptrdiff_t
  >
  class iterator_facade
# ifdef BOOST_ITERATOR_FACADE_NEEDS_ITERATOR_BASE
    : public boost::detail::iterator_facade_types<
         Value, CategoryOrTraversal, Reference, Difference
      >::base
#  undef BOOST_ITERATOR_FACADE_NEEDS_ITERATOR_BASE
# endif
  {
   private:
      //
      // Curiously Recurring Template interface.
      //
      Derived& derived()
      {
          return *static_cast<Derived*>(this);
      }

      Derived const& derived() const
      {
          return *static_cast<Derived const*>(this);
      }

      typedef boost::detail::iterator_facade_types<
         Value, CategoryOrTraversal, Reference, Difference
      > associated_types;

      typedef boost::detail::operator_arrow_result<
        typename associated_types::value_type
        , Reference
        , typename associated_types::pointer
      > pointer_;

   protected:
      // For use by derived classes
      typedef iterator_facade<Derived,Value,CategoryOrTraversal,Reference,Difference> iterator_facade_;
      
   public:

      typedef typename associated_types::value_type value_type;
      typedef Reference reference;
      typedef Difference difference_type;

      typedef typename pointer_::type pointer;

      typedef typename associated_types::iterator_category iterator_category;

      reference operator*() const
      {
          return iterator_core_access::dereference(this->derived());
      }

      pointer operator->() const
      {
          return pointer_::make(*this->derived());
      }
        
      typename boost::detail::operator_brackets_result<Derived,Value,reference>::type
      operator[](difference_type n) const
      {
          typedef boost::detail::use_operator_brackets_proxy<Value,Reference> use_proxy;
          
          return boost::detail::make_operator_brackets_result<Derived>(
              this->derived() + n
            , use_proxy()
          );
      }

      Derived& operator++()
      {
          iterator_core_access::increment(this->derived());
          return this->derived();
      }

# if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
      typename boost::detail::postfix_increment_result<Derived,Value,Reference,CategoryOrTraversal>::type
      operator++(int)
      {
          typename boost::detail::postfix_increment_result<Derived,Value,Reference,CategoryOrTraversal>::type
          tmp(this->derived());
          ++*this;
          return tmp;
      }
# endif
      
      Derived& operator--()
      {
          iterator_core_access::decrement(this->derived());
          return this->derived();
      }

      Derived operator--(int)
      {
          Derived tmp(this->derived());
          --*this;
          return tmp;
      }

      Derived& operator+=(difference_type n)
      {
          iterator_core_access::advance(this->derived(), n);
          return this->derived();
      }

      Derived& operator-=(difference_type n)
      {
          iterator_core_access::advance(this->derived(), -n);
          return this->derived();
      }

      Derived operator-(difference_type x) const
      {
          Derived result(this->derived());
          return result -= x;
      }

# if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
      // There appears to be a bug which trashes the data of classes
      // derived from iterator_facade when they are assigned unless we
      // define this assignment operator.  This bug is only revealed
      // (so far) in STLPort debug mode, but it's clearly a codegen
      // problem so we apply the workaround for all MSVC6.
      iterator_facade& operator=(iterator_facade const&)
      {
          return *this;
      }
# endif
  };

# if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)
  template <class I, class V, class TC, class R, class D>
  inline typename boost::detail::postfix_increment_result<I,V,R,TC>::type
  operator++(
      iterator_facade<I,V,TC,R,D>& i
    , int
  )
  {
      typename boost::detail::postfix_increment_result<I,V,R,TC>::type
          tmp(*static_cast<I*>(&i));
      
      ++i;
      
      return tmp;
  }
# endif 

  
  //
  // Comparison operator implementation. The library supplied operators
  // enables the user to provide fully interoperable constant/mutable
  // iterator types. I.e. the library provides all operators
  // for all mutable/constant iterator combinations.
  //
  // Note though that this kind of interoperability for constant/mutable
  // iterators is not required by the standard for container iterators.
  // All the standard asks for is a conversion mutable -> constant.
  // Most standard library implementations nowadays provide fully interoperable
  // iterator implementations, but there are still heavily used implementations
  // that do not provide them. (Actually it's even worse, they do not provide
  // them for only a few iterators.)
  //
  // ?? Maybe a BOOST_ITERATOR_NO_FULL_INTEROPERABILITY macro should
  //    enable the user to turn off mixed type operators
  //
  // The library takes care to provide only the right operator overloads.
  // I.e.
  //
  // bool operator==(Iterator,      Iterator);
  // bool operator==(ConstIterator, Iterator);
  // bool operator==(Iterator,      ConstIterator);
  // bool operator==(ConstIterator, ConstIterator);
  //
  //   ...
  //
  // In order to do so it uses c++ idioms that are not yet widely supported
  // by current compiler releases. The library is designed to degrade gracefully
  // in the face of compiler deficiencies. In general compiler
  // deficiencies result in less strict error checking and more obscure
  // error messages, functionality is not affected.
  //
  // For full operation compiler support for "Substitution Failure Is Not An Error"
  // (aka. enable_if) and boost::is_convertible is required.
  //
  // The following problems occur if support is lacking.
  //
  // Pseudo code
  //
  // ---------------
  // AdaptorA<Iterator1> a1;
  // AdaptorA<Iterator2> a2;
  //
  // // This will result in a no such overload error in full operation
  // // If enable_if or is_convertible is not supported
  // // The instantiation will fail with an error hopefully indicating that
  // // there is no operator== for Iterator1, Iterator2
  // // The same will happen if no enable_if is used to remove
  // // false overloads from the templated conversion constructor
  // // of AdaptorA.
  //
  // a1 == a2;
  // ----------------
  //
  // AdaptorA<Iterator> a;
  // AdaptorB<Iterator> b;
  //
  // // This will result in a no such overload error in full operation
  // // If enable_if is not supported the static assert used
  // // in the operator implementation will fail.
  // // This will accidently work if is_convertible is not supported.
  //
  // a == b;
  // ----------------
  //

# ifdef BOOST_NO_ONE_WAY_ITERATOR_INTEROP
#  define BOOST_ITERATOR_CONVERTIBLE(a,b) mpl::true_()
# else
#  define BOOST_ITERATOR_CONVERTIBLE(a,b) is_convertible<a,b>()
# endif

# define BOOST_ITERATOR_FACADE_INTEROP(op, result_type, return_prefix, base_op) \
  BOOST_ITERATOR_FACADE_INTEROP_HEAD(inline, op, result_type)                   \
  {                                                                             \
      /* For those compilers that do not support enable_if */                   \
      BOOST_STATIC_ASSERT((                                                     \
          is_interoperable< Derived1, Derived2 >::value                         \
      ));                                                                       \
      return_prefix iterator_core_access::base_op(                              \
          *static_cast<Derived1 const*>(&lhs)                                   \
        , *static_cast<Derived2 const*>(&rhs)                                   \
        , BOOST_ITERATOR_CONVERTIBLE(Derived2,Derived1)                         \
      );                                                                        \
  }

# define BOOST_ITERATOR_FACADE_RELATION(op, return_prefix, base_op) \
  BOOST_ITERATOR_FACADE_INTEROP(                                    \
      op                                                            \
    , boost::detail::always_bool2                                   \
    , return_prefix                                                 \
    , base_op                                                       \
  )

  BOOST_ITERATOR_FACADE_RELATION(==, return, equal)
  BOOST_ITERATOR_FACADE_RELATION(!=, return !, equal)

  BOOST_ITERATOR_FACADE_RELATION(<, return 0 >, distance_from)
  BOOST_ITERATOR_FACADE_RELATION(>, return 0 <, distance_from)
  BOOST_ITERATOR_FACADE_RELATION(<=, return 0 >=, distance_from)
  BOOST_ITERATOR_FACADE_RELATION(>=, return 0 <=, distance_from)
# undef BOOST_ITERATOR_FACADE_RELATION

  // operator- requires an additional part in the static assertion
  BOOST_ITERATOR_FACADE_INTEROP(
      -
    , boost::detail::choose_difference_type
    , return
    , distance_from
  )
# undef BOOST_ITERATOR_FACADE_INTEROP
# undef BOOST_ITERATOR_FACADE_INTEROP_HEAD

# define BOOST_ITERATOR_FACADE_PLUS(args)           \
  BOOST_ITERATOR_FACADE_PLUS_HEAD(inline, args)     \
  {                                                 \
      Derived tmp(static_cast<Derived const&>(i));  \
      return tmp += n;                              \
  }

BOOST_ITERATOR_FACADE_PLUS((
  iterator_facade<Derived, V, TC, R, D> const& i
  , typename Derived::difference_type n
))

BOOST_ITERATOR_FACADE_PLUS((
    typename Derived::difference_type n
    , iterator_facade<Derived, V, TC, R, D> const& i
))
# undef BOOST_ITERATOR_FACADE_PLUS
# undef BOOST_ITERATOR_FACADE_PLUS_HEAD

} // namespace boost

#include <boost/iterator/detail/config_undef.hpp>

#endif // BOOST_ITERATOR_FACADE_23022003THW_HPP
