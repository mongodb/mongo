// Copyright David Abrahams and Thomas Becker 2000-2006. Distributed
// under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ZIP_ITERATOR_TMB_07_13_2003_HPP_
# define BOOST_ZIP_ITERATOR_TMB_07_13_2003_HPP_

#include <stddef.h>
#include <boost/iterator.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/iterator_adaptor.hpp> // for enable_if_convertible
#include <boost/iterator/iterator_categories.hpp>
#include <boost/detail/iterator.hpp>

#include <boost/iterator/detail/minimum_category.hpp>

#include <boost/tuple/tuple.hpp>

#include <boost/type_traits/is_same.hpp>
#include <boost/mpl/and.hpp>
#include <boost/mpl/apply.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/lambda.hpp>
#include <boost/mpl/placeholders.hpp>
#include <boost/mpl/aux_/lambda_support.hpp>

namespace boost {

  // Zip iterator forward declaration for zip_iterator_base
  template<typename IteratorTuple>
  class zip_iterator;

  // One important design goal of the zip_iterator is to isolate all
  // functionality whose implementation relies on the current tuple
  // implementation. This goal has been achieved as follows: Inside
  // the namespace detail there is a namespace tuple_impl_specific.
  // This namespace encapsulates all functionality that is specific
  // to the current Boost tuple implementation. More precisely, the
  // namespace tuple_impl_specific provides the following tuple
  // algorithms and meta-algorithms for the current Boost tuple
  // implementation:
  //
  // tuple_meta_transform
  // tuple_meta_accumulate
  // tuple_transform
  // tuple_for_each
  //
  // If the tuple implementation changes, all that needs to be
  // replaced is the implementation of these four (meta-)algorithms.

  namespace detail
  {

    // Functors to be used with tuple algorithms
    //
    template<typename DiffType>
    class advance_iterator
    {
    public:
      advance_iterator(DiffType step) : m_step(step) {}
      
      template<typename Iterator>
      void operator()(Iterator& it) const
      { it += m_step; }

    private:
      DiffType m_step;
    };
    //
    struct increment_iterator
    {
      template<typename Iterator>
      void operator()(Iterator& it)
      { ++it; }
    };
    //
    struct decrement_iterator
    {
      template<typename Iterator>
      void operator()(Iterator& it)
      { --it; }
    };
    //
    struct dereference_iterator
    {
      template<typename Iterator>
      struct apply
      { 
        typedef typename
          iterator_traits<Iterator>::reference
        type;
      };

      template<typename Iterator>
        typename apply<Iterator>::type operator()(Iterator const& it)
      { return *it; }
    };
           

    // The namespace tuple_impl_specific provides two meta-
    // algorithms and two algorithms for tuples.
    //
    namespace tuple_impl_specific
    {
      // Meta-transform algorithm for tuples
      //
      template<typename Tuple, class UnaryMetaFun>
      struct tuple_meta_transform;
      
      template<typename Tuple, class UnaryMetaFun>
      struct tuple_meta_transform_impl
      {
          typedef tuples::cons<
              typename mpl::apply1<
                  typename mpl::lambda<UnaryMetaFun>::type
                , typename Tuple::head_type
              >::type
            , typename tuple_meta_transform<
                  typename Tuple::tail_type
                , UnaryMetaFun 
              >::type
          > type;
      };

      template<typename Tuple, class UnaryMetaFun>
      struct tuple_meta_transform
        : mpl::eval_if<
              boost::is_same<Tuple, tuples::null_type>
            , mpl::identity<tuples::null_type>
            , tuple_meta_transform_impl<Tuple, UnaryMetaFun>
        >
      {
      };
      
      // Meta-accumulate algorithm for tuples. Note: The template 
      // parameter StartType corresponds to the initial value in 
      // ordinary accumulation.
      //
      template<class Tuple, class BinaryMetaFun, class StartType>
      struct tuple_meta_accumulate;
      
      template<
          typename Tuple
        , class BinaryMetaFun
        , typename StartType
      >
      struct tuple_meta_accumulate_impl
      {
         typedef typename mpl::apply2<
             typename mpl::lambda<BinaryMetaFun>::type
           , typename Tuple::head_type
           , typename tuple_meta_accumulate<
                 typename Tuple::tail_type
               , BinaryMetaFun
               , StartType 
             >::type
         >::type type;
      };

      template<
          typename Tuple
        , class BinaryMetaFun
        , typename StartType
      >
      struct tuple_meta_accumulate
        : mpl::eval_if<
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
              mpl::or_<
#endif 
                  boost::is_same<Tuple, tuples::null_type>
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
                , boost::is_same<Tuple,int>
              >
#endif 
            , mpl::identity<StartType>
            , tuple_meta_accumulate_impl<
                  Tuple
                , BinaryMetaFun
                , StartType
              >
          >
      {
      };  

#if defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)                            \
    || (                                                                    \
      BOOST_WORKAROUND(BOOST_INTEL_CXX_VERSION, != 0) && defined(_MSC_VER)  \
    )
// Not sure why intel's partial ordering fails in this case, but I'm
// assuming int's an MSVC bug-compatibility feature.
      
# define BOOST_TUPLE_ALGO_DISPATCH
# define BOOST_TUPLE_ALGO(algo) algo##_impl
# define BOOST_TUPLE_ALGO_TERMINATOR , int
# define BOOST_TUPLE_ALGO_RECURSE , ...
#else 
# define BOOST_TUPLE_ALGO(algo) algo
# define BOOST_TUPLE_ALGO_TERMINATOR
# define BOOST_TUPLE_ALGO_RECURSE
#endif
      
      // transform algorithm for tuples. The template parameter Fun
      // must be a unary functor which is also a unary metafunction
      // class that computes its return type based on its argument
      // type. For example:
      //
      // struct to_ptr
      // {
      //     template <class Arg>
      //     struct apply
      //     {
      //          typedef Arg* type;
      //     }
      //
      //     template <class Arg>
      //     Arg* operator()(Arg x);
      // };
      template<typename Fun>
      tuples::null_type BOOST_TUPLE_ALGO(tuple_transform)
          (tuples::null_type const&, Fun BOOST_TUPLE_ALGO_TERMINATOR)
      { return tuples::null_type(); }

      template<typename Tuple, typename Fun>
      typename tuple_meta_transform<
          Tuple
        , Fun
      >::type
      
      BOOST_TUPLE_ALGO(tuple_transform)(
        const Tuple& t, 
        Fun f
        BOOST_TUPLE_ALGO_RECURSE
      )
      { 
          typedef typename tuple_meta_transform<
              BOOST_DEDUCED_TYPENAME Tuple::tail_type
            , Fun
          >::type transformed_tail_type;

        return tuples::cons<
            BOOST_DEDUCED_TYPENAME mpl::apply1<
                Fun, BOOST_DEDUCED_TYPENAME Tuple::head_type
             >::type
           , transformed_tail_type
        >( 
            f(boost::tuples::get<0>(t)), tuple_transform(t.get_tail(), f)
        );
      }

#ifdef BOOST_TUPLE_ALGO_DISPATCH
      template<typename Tuple, typename Fun>
      typename tuple_meta_transform<
          Tuple
        , Fun
      >::type
      
      tuple_transform(
        const Tuple& t, 
        Fun f
      )
      {
          return tuple_transform_impl(t, f, 1);
      }
#endif
      
      // for_each algorithm for tuples.
      //
      template<typename Fun>
      Fun BOOST_TUPLE_ALGO(tuple_for_each)(
          tuples::null_type
        , Fun f BOOST_TUPLE_ALGO_TERMINATOR
      )
      { return f; }

      
      template<typename Tuple, typename Fun>
      Fun BOOST_TUPLE_ALGO(tuple_for_each)(
          Tuple& t
        , Fun f BOOST_TUPLE_ALGO_RECURSE)
      { 
          f( t.get_head() );
          return tuple_for_each(t.get_tail(), f);
      }
      
#ifdef BOOST_TUPLE_ALGO_DISPATCH
      template<typename Tuple, typename Fun>
      Fun
      tuple_for_each(
        Tuple& t, 
        Fun f
      )
      {
          return tuple_for_each_impl(t, f, 1);
      }
#endif
      
      // Equality of tuples. NOTE: "==" for tuples currently (7/2003)
      // has problems under some compilers, so I just do my own.
      // No point in bringing in a bunch of #ifdefs here. This is
      // going to go away with the next tuple implementation anyway.
      //
      inline bool tuple_equal(tuples::null_type, tuples::null_type)
      { return true; }

      template<typename Tuple1, typename Tuple2>
        bool tuple_equal(
            Tuple1 const& t1, 
            Tuple2 const& t2
        )
      { 
          return t1.get_head() == t2.get_head() && 
          tuple_equal(t1.get_tail(), t2.get_tail());
      }
    }
    //
    // end namespace tuple_impl_specific

    template<typename Iterator>
    struct iterator_reference
    {
        typedef typename iterator_traits<Iterator>::reference type;
    };

#ifdef BOOST_MPL_CFG_NO_FULL_LAMBDA_SUPPORT
    // Hack because BOOST_MPL_AUX_LAMBDA_SUPPORT doesn't seem to work
    // out well.  Instantiating the nested apply template also
    // requires instantiating iterator_traits on the
    // placeholder. Instead we just specialize it as a metafunction
    // class.
    template<>
    struct iterator_reference<mpl::_1>
    {
        template <class T>
        struct apply : iterator_reference<T> {};
    };
#endif
    
    // Metafunction to obtain the type of the tuple whose element types
    // are the reference types of an iterator tuple.
    //
    template<typename IteratorTuple>
    struct tuple_of_references
      : tuple_impl_specific::tuple_meta_transform<
            IteratorTuple, 
            iterator_reference<mpl::_1>
          >
    {
    };

    // Metafunction to obtain the minimal traversal tag in a tuple
    // of iterators.
    //
    template<typename IteratorTuple>
    struct minimum_traversal_category_in_iterator_tuple
    {
      typedef typename tuple_impl_specific::tuple_meta_transform<
          IteratorTuple
        , iterator_traversal<>
      >::type tuple_of_traversal_tags;
          
      typedef typename tuple_impl_specific::tuple_meta_accumulate<
          tuple_of_traversal_tags
        , minimum_category<>
        , random_access_traversal_tag
      >::type type;
    };

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300) // ETI workaround
      template <>
      struct minimum_traversal_category_in_iterator_tuple<int>
      {
          typedef int type;
      };
#endif
      
      // We need to call tuple_meta_accumulate with mpl::and_ as the
      // accumulating functor. To this end, we need to wrap it into
      // a struct that has exactly two arguments (that is, template
      // parameters) and not five, like mpl::and_ does.
      //
      template<typename Arg1, typename Arg2>
      struct and_with_two_args
        : mpl::and_<Arg1, Arg2>
      {
      };
    
# ifdef BOOST_MPL_CFG_NO_FULL_LAMBDA_SUPPORT
  // Hack because BOOST_MPL_AUX_LAMBDA_SUPPORT doesn't seem to work
  // out well.  In this case I think it's an MPL bug
      template<>
      struct and_with_two_args<mpl::_1,mpl::_2>
      {
          template <class A1, class A2>
          struct apply : mpl::and_<A1,A2>
          {};
      };
# endif 

    ///////////////////////////////////////////////////////////////////
    //
    // Class zip_iterator_base
    //
    // Builds and exposes the iterator facade type from which the zip 
    // iterator will be derived.
    //
    template<typename IteratorTuple>
    struct zip_iterator_base
    {
     private:
        // Reference type is the type of the tuple obtained from the
        // iterators' reference types.
        typedef typename 
        detail::tuple_of_references<IteratorTuple>::type reference;
      
        // Value type is the same as reference type.
        typedef reference value_type;
      
        // Difference type is the first iterator's difference type
        typedef typename iterator_traits<
            typename tuples::element<0, IteratorTuple>::type
            >::difference_type difference_type;
      
        // Traversal catetgory is the minimum traversal category in the 
        // iterator tuple.
        typedef typename 
        detail::minimum_traversal_category_in_iterator_tuple<
            IteratorTuple
        >::type traversal_category;
     public:
      
        // The iterator facade type from which the zip iterator will
        // be derived.
        typedef iterator_facade<
            zip_iterator<IteratorTuple>,
            value_type,  
            traversal_category,
            reference,
            difference_type
        > type;
    };

    template <>
    struct zip_iterator_base<int>
    {
        typedef int type;
    };
  }
  
  /////////////////////////////////////////////////////////////////////
  //
  // zip_iterator class definition
  //
  template<typename IteratorTuple>
  class zip_iterator : 
    public detail::zip_iterator_base<IteratorTuple>::type
  {  

   // Typedef super_t as our base class. 
   typedef typename 
     detail::zip_iterator_base<IteratorTuple>::type super_t;

   // iterator_core_access is the iterator's best friend.
   friend class iterator_core_access;

  public:
    
    // Construction
    // ============
    
    // Default constructor
    zip_iterator() { }

    // Constructor from iterator tuple
    zip_iterator(IteratorTuple iterator_tuple) 
      : m_iterator_tuple(iterator_tuple) 
    { }

    // Copy constructor
    template<typename OtherIteratorTuple>
    zip_iterator(
       const zip_iterator<OtherIteratorTuple>& other,
       typename enable_if_convertible<
         OtherIteratorTuple,
         IteratorTuple
         >::type* = 0
    ) : m_iterator_tuple(other.get_iterator_tuple())
    {}

    // Get method for the iterator tuple.
    const IteratorTuple& get_iterator_tuple() const
    { return m_iterator_tuple; }

  private:
    
    // Implementation of Iterator Operations
    // =====================================
    
    // Dereferencing returns a tuple built from the dereferenced
    // iterators in the iterator tuple.
    typename super_t::reference dereference() const
    { 
      return detail::tuple_impl_specific::tuple_transform( 
        get_iterator_tuple(),
        detail::dereference_iterator()
       );
    }

    // Two zip iterators are equal if all iterators in the iterator
    // tuple are equal. NOTE: It should be possible to implement this
    // as
    //
    // return get_iterator_tuple() == other.get_iterator_tuple();
    //
    // but equality of tuples currently (7/2003) does not compile
    // under several compilers. No point in bringing in a bunch
    // of #ifdefs here.
    //
    template<typename OtherIteratorTuple>   
    bool equal(const zip_iterator<OtherIteratorTuple>& other) const
    {
      return detail::tuple_impl_specific::tuple_equal(
        get_iterator_tuple(),
        other.get_iterator_tuple()
        );
    }

    // Advancing a zip iterator means to advance all iterators in the
    // iterator tuple.
    void advance(typename super_t::difference_type n)
    { 
      detail::tuple_impl_specific::tuple_for_each(
          m_iterator_tuple,
          detail::advance_iterator<BOOST_DEDUCED_TYPENAME super_t::difference_type>(n)
          );
    }
    // Incrementing a zip iterator means to increment all iterators in
    // the iterator tuple.
    void increment()
    { 
      detail::tuple_impl_specific::tuple_for_each(
        m_iterator_tuple,
        detail::increment_iterator()
        );
    }
    
    // Decrementing a zip iterator means to decrement all iterators in
    // the iterator tuple.
    void decrement()
    { 
      detail::tuple_impl_specific::tuple_for_each(
        m_iterator_tuple,
        detail::decrement_iterator()
        );
    }
    
    // Distance is calculated using the first iterator in the tuple.
    template<typename OtherIteratorTuple>
      typename super_t::difference_type distance_to(
        const zip_iterator<OtherIteratorTuple>& other
        ) const
    { 
        return boost::tuples::get<0>(other.get_iterator_tuple()) - 
            boost::tuples::get<0>(this->get_iterator_tuple());
    }
  
    // Data Members
    // ============
    
    // The iterator tuple.
    IteratorTuple m_iterator_tuple;
 
  };

  // Make function for zip iterator
  //
  template<typename IteratorTuple> 
  zip_iterator<IteratorTuple> 
  make_zip_iterator(IteratorTuple t)
  { return zip_iterator<IteratorTuple>(t); }

}

#endif
