// Copyright (C) 2003, 2008 Fernando Luis Cacciola Carballal.
// Copyright (C) 2014 - 2021 Andrzej Krzemienski.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  fernando_cacciola@hotmail.com
//
// Revisions:
// 27 Apr 2008 (improved swap) Fernando Cacciola, Niels Dekker, Thorsten Ottosen
// 05 May 2014 (Added move semantics) Andrzej Krzemienski
//
#ifndef BOOST_OPTIONAL_OPTIONAL_FLC_19NOV2002_HPP
#define BOOST_OPTIONAL_OPTIONAL_FLC_19NOV2002_HPP

#include <new>
#ifndef BOOST_NO_IOSTREAM
#include <iosfwd>
#endif // BOOST_NO_IOSTREAM

#include <boost/assert.hpp>
#include <boost/core/addressof.hpp>
#include <boost/core/enable_if.hpp>
#include <boost/core/invoke_swap.hpp>
#include <boost/core/launder.hpp>
#include <boost/optional/bad_optional_access.hpp>
#include <boost/throw_exception.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/type_traits/conditional.hpp>
#include <boost/type_traits/conjunction.hpp>
#include <boost/type_traits/disjunction.hpp>
#include <boost/type_traits/has_nothrow_constructor.hpp>
#include <boost/type_traits/type_with_alignment.hpp>
#include <boost/type_traits/remove_const.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/type_traits/decay.hpp>
#include <boost/type_traits/is_assignable.hpp>
#include <boost/type_traits/is_base_of.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/is_constructible.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_lvalue_reference.hpp>
#include <boost/type_traits/is_nothrow_move_assignable.hpp>
#include <boost/type_traits/is_nothrow_move_constructible.hpp>
#include <boost/type_traits/is_rvalue_reference.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/is_volatile.hpp>
#include <boost/type_traits/is_scalar.hpp>
#include <boost/none.hpp>

#include <boost/optional/optional_fwd.hpp>
#include <boost/optional/detail/optional_config.hpp>
#include <boost/optional/detail/optional_factory_support.hpp>
#include <boost/optional/detail/optional_aligned_storage.hpp>
#include <boost/optional/detail/optional_hash.hpp>
#include <boost/optional/detail/optional_utility.hpp>

namespace boost { namespace optional_detail {

template <typename T>
struct optional_value_type
{
};

template <typename U>
struct optional_value_type< ::boost::optional<U> >
{
  typedef U type;
};

template <typename T>
T declval();


// implementing my own result_of so that it works for C++11 (std::result_of)
// and in C++20 (std::invoke_result).
template <typename F, typename Ref, typename Rslt = decltype(declval<F>()(declval<Ref>()))>
struct result_of
{
  typedef Rslt type;
};

template <typename F, typename Ref, typename Rslt = typename optional_value_type<typename result_of<F, Ref>::type>::type>
struct result_value_type
{
  typedef Rslt type;
};

// optional<typename optional_detail::optional_value_type<decltype(optional_detail::declval<F>()(optional_detail::declval<reference_type>()))>::type>

}} // namespace boost::optional_detail

namespace boost {

namespace optional_ns {

// a tag for in-place initialization of contained value
struct in_place_init_t
{
  struct init_tag{};
  BOOST_CONSTEXPR explicit in_place_init_t(init_tag){}
};
BOOST_INLINE_CONSTEXPR in_place_init_t in_place_init ((in_place_init_t::init_tag()));

// a tag for conditional in-place initialization of contained value
struct in_place_init_if_t
{
  struct init_tag{};
  BOOST_CONSTEXPR explicit in_place_init_if_t(init_tag){}
};
BOOST_INLINE_CONSTEXPR in_place_init_if_t in_place_init_if ((in_place_init_if_t::init_tag()));

} // namespace optional_ns

using optional_ns::in_place_init_t;
using optional_ns::in_place_init;
using optional_ns::in_place_init_if_t;
using optional_ns::in_place_init_if;

namespace optional_detail {

struct init_value_tag {};

struct optional_tag {};


template<class T>
class optional_base : public optional_tag
{
  private :

    typedef aligned_storage<T> storage_type ;
    typedef optional_base<T> this_type ;

  protected :

    typedef T value_type ;
    typedef typename boost::remove_const<T>::type unqualified_value_type;

  protected:
    typedef T &       reference_type ;
    typedef T const&  reference_const_type ;
    typedef T &&  rval_reference_type ;
    typedef T &&  reference_type_of_temporary_wrapper ;
    typedef T *         pointer_type ;
    typedef T const*    pointer_const_type ;
    typedef T const&    argument_type ;

    // Creates an optional<T> uninitialized.
    // No-throw
    optional_base()
      :
      m_initialized(false) {}

    // Creates an optional<T> uninitialized.
    // No-throw
    optional_base ( none_t )
      :
      m_initialized(false) {}

    // Creates an optional<T> initialized with 'val'.
    // Can throw if T::T(T const&) does
    optional_base ( init_value_tag, argument_type val )
      :
      m_initialized(false)
    {
        construct(val);
    }

    // move-construct an optional<T> initialized from an rvalue-ref to 'val'.
    // Can throw if T::T(T&&) does
    optional_base ( init_value_tag, rval_reference_type val )
      :
      m_initialized(false)
    {
      construct( optional_detail::move(val) );
    }

    // Creates an optional<T> initialized with 'val' IFF cond is true, otherwise creates an uninitialized optional<T>.
    // Can throw if T::T(T const&) does
    optional_base ( bool cond, argument_type val )
      :
      m_initialized(false)
    {
      if ( cond )
        construct(val);
    }

    // Creates an optional<T> initialized with 'move(val)' IFF cond is true, otherwise creates an uninitialized optional<T>.
    // Can throw if T::T(T &&) does
    optional_base ( bool cond, rval_reference_type val )
      :
      m_initialized(false)
    {
      if ( cond )
        construct(optional_detail::move(val));
    }

    // Creates a deep copy of another optional<T>
    // Can throw if T::T(T const&) does
    optional_base ( optional_base const& rhs )
      :
      m_initialized(false)
    {
      if ( rhs.is_initialized() )
        construct(rhs.get_impl());
    }

    // Creates a deep move of another optional<T>
    // Can throw if T::T(T&&) does
    optional_base ( optional_base&& rhs )
    BOOST_NOEXCEPT_IF(::boost::is_nothrow_move_constructible<T>::value)
      :
      m_initialized(false)
    {
      if ( rhs.is_initialized() )
        construct( optional_detail::move(rhs.get_impl()) );
    }


    template<class Expr, class PtrExpr>
    explicit optional_base ( Expr&& expr, PtrExpr const* tag )
      :
      m_initialized(false)
    {
      construct(optional_detail::forward<Expr>(expr),tag);
    }

    optional_base& operator= ( optional_base const& rhs )
    {
      this->assign(rhs);
      return *this;
    }

    optional_base& operator= ( optional_base && rhs )
    BOOST_NOEXCEPT_IF(::boost::is_nothrow_move_constructible<T>::value && ::boost::is_nothrow_move_assignable<T>::value)
    {
      this->assign(static_cast<optional_base&&>(rhs));
      return *this;
    }

    // No-throw (assuming T::~T() doesn't)
    ~optional_base() { destroy() ; }

    // Assigns from another optional<T> (deep-copies the rhs value)
    void assign ( optional_base const& rhs )
    {
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
             assign_value(rhs.get_impl());
        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
          construct(rhs.get_impl());
      }
    }

    // Assigns from another optional<T> (deep-moves the rhs value)
    void assign ( optional_base&& rhs )
    {
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
             assign_value( optional_detail::move(rhs.get_impl()) );
        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
          construct(optional_detail::move(rhs.get_impl()));
      }
    }

    // Assigns from another _convertible_ optional<U> (deep-copies the rhs value)
    template<class U>
    void assign ( optional<U> const& rhs )
    {
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
#ifndef BOOST_OPTIONAL_CONFIG_RESTORE_ASSIGNMENT_OF_NONCONVERTIBLE_TYPES
          assign_value( rhs.get() );
#else
          assign_value( static_cast<value_type>(rhs.get()) );
#endif

        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
#ifndef BOOST_OPTIONAL_CONFIG_RESTORE_ASSIGNMENT_OF_NONCONVERTIBLE_TYPES
          construct(rhs.get());
#else
          construct(static_cast<value_type>(rhs.get()));
#endif
      }
    }

    // move-assigns from another _convertible_ optional<U> (deep-moves from the rhs value)
    template<class U>
    void assign ( optional<U>&& rhs )
    {
      typedef BOOST_DEDUCED_TYPENAME optional<U>::rval_reference_type ref_type;
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
             assign_value( static_cast<ref_type>(rhs.get()) );
        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
          construct(static_cast<ref_type>(rhs.get()));
      }
    }

    // Assigns from a T (deep-copies the rhs value)
    void assign ( argument_type val )
    {
      if (is_initialized())
           assign_value(val);
      else construct(val);
    }

    // Assigns from a T (deep-moves the rhs value)
    void assign ( rval_reference_type val )
    {
      if (is_initialized())
           assign_value( optional_detail::move(val) );
      else construct( optional_detail::move(val) );
    }

    // Assigns from "none", destroying the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    void assign ( none_t ) BOOST_NOEXCEPT { destroy(); }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT

    template<class Expr, class ExprPtr>
    void assign_expr ( Expr&& expr, ExprPtr const* tag )
    {
      if (is_initialized())
        assign_expr_to_initialized(optional_detail::forward<Expr>(expr),tag);
      else construct(optional_detail::forward<Expr>(expr),tag);
    }

#endif

  public :

    // Destroys the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    void reset() BOOST_NOEXCEPT { destroy(); }

    // **DEPRECATED** Replaces the current value -if any- with 'val'
    void reset ( argument_type val ) { assign(val); }

    // Returns a pointer to the value if this is initialized, otherwise,
    // returns NULL.
    // No-throw
    pointer_const_type get_ptr() const { return m_initialized ? get_ptr_impl() : 0 ; }
    pointer_type       get_ptr()       { return m_initialized ? get_ptr_impl() : 0 ; }

    bool is_initialized() const BOOST_NOEXCEPT { return m_initialized ; }

  protected :

    void construct ( argument_type val )
     {
       ::new (m_storage.address()) unqualified_value_type(val) ;
       m_initialized = true ;
     }

    void construct ( rval_reference_type val )
     {
       ::new (m_storage.address()) unqualified_value_type( optional_detail::move(val) ) ;
       m_initialized = true ;
     }


    // Constructs in-place
    // upon exception *this is always uninitialized
    template<class... Args>
    void construct ( in_place_init_t, Args&&... args )
    {
      ::new (m_storage.address()) unqualified_value_type( optional_detail::forward<Args>(args)... ) ;
      m_initialized = true ;
    }

    template<class... Args>
    void emplace_assign ( Args&&... args )
    {
      destroy();
      construct(in_place_init, optional_detail::forward<Args>(args)...);
    }

    template<class... Args>
    explicit optional_base ( in_place_init_t, Args&&... args )
      :
      m_initialized(false)
    {
      construct(in_place_init, optional_detail::forward<Args>(args)...);
    }

    template<class... Args>
    explicit optional_base ( in_place_init_if_t, bool cond, Args&&... args )
      :
      m_initialized(false)
    {
      if ( cond )
        construct(in_place_init, optional_detail::forward<Args>(args)...);
    }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT

    // Constructs in-place using the given factory
    template<class Expr>
    void construct ( Expr&& factory, in_place_factory_base const* )
     {
       boost_optional_detail::construct<value_type>(factory, m_storage.address());
       m_initialized = true ;
     }

    // Constructs in-place using the given typed factory
    template<class Expr>
    void construct ( Expr&& factory, typed_in_place_factory_base const* )
     {
       factory.apply(m_storage.address()) ;
       m_initialized = true ;
     }

    template<class Expr>
    void assign_expr_to_initialized ( Expr&& factory, in_place_factory_base const* tag )
     {
       destroy();
       construct(factory,tag);
     }

    // Constructs in-place using the given typed factory
    template<class Expr>
    void assign_expr_to_initialized ( Expr&& factory, typed_in_place_factory_base const* tag )
     {
       destroy();
       construct(factory,tag);
     }

#endif

    // Constructs using any expression implicitly convertible to the single argument
    // of a one-argument T constructor.
    // Converting constructions of optional<T> from optional<U> uses this function with
    // 'Expr' being of type 'U' and relying on a converting constructor of T from U.
    template<class Expr>
    void construct ( Expr&& expr, void const* )
    {
      new (m_storage.address()) unqualified_value_type(optional_detail::forward<Expr>(expr)) ;
      m_initialized = true ;
    }

    // Assigns using a form any expression implicitly convertible to the single argument
    // of a T's assignment operator.
    // Converting assignments of optional<T> from optional<U> uses this function with
    // 'Expr' being of type 'U' and relying on a converting assignment of T from U.
    template<class Expr>
    void assign_expr_to_initialized ( Expr&& expr, void const* )
    {
      assign_value( optional_detail::forward<Expr>(expr) );
    }

#ifdef BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION
    // BCB5.64 (and probably lower versions) workaround.
    //   The in-place factories are supported by means of catch-all constructors
    //   and assignment operators (the functions are parameterized in terms of
    //   an arbitrary 'Expr' type)
    //   This compiler incorrectly resolves the overload set and sinks optional<T> and optional<U>
    //   to the 'Expr'-taking functions even though explicit overloads are present for them.
    //   Thus, the following overload is needed to properly handle the case when the 'lhs'
    //   is another optional.
    //
    // For VC<=70 compilers this workaround doesn't work because the compiler issues and error
    // instead of choosing the wrong overload
    //

    // Notice that 'Expr' will be optional<T> or optional<U> (but not optional_base<..>)
    template<class Expr>
    void construct ( Expr&& expr, optional_tag const* )
     {
       if ( expr.is_initialized() )
       {
         // An exception can be thrown here.
         // It it happens, THIS will be left uninitialized.
         new (m_storage.address()) unqualified_value_type(optional_detail::move(expr.get())) ;
         m_initialized = true ;
       }
     }
#endif // defined BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION

    void assign_value ( argument_type val ) { get_impl() = val; }
    void assign_value ( rval_reference_type val ) { get_impl() = static_cast<rval_reference_type>(val); }

    void destroy()
    {
      if ( m_initialized )
        destroy_impl() ;
    }

    reference_const_type get_impl() const { return m_storage.ref() ; }
    reference_type       get_impl()       { return m_storage.ref() ; }

    pointer_const_type get_ptr_impl() const { return m_storage.ptr_ref(); }
    pointer_type       get_ptr_impl()       { return m_storage.ptr_ref(); }

  private :

#if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1900))
    void destroy_impl ( ) { m_storage.ptr_ref()->~T() ; m_initialized = false ; }
#else
    void destroy_impl ( ) { m_storage.ref().T::~T() ; m_initialized = false ; }
#endif

    bool m_initialized ;
    storage_type m_storage ;
} ;

#include <boost/optional/detail/optional_trivially_copyable_base.hpp>

// definition of metafunction is_optional_val_init_candidate
template <typename U>
struct is_optional_or_tag
  : boost::conditional< boost::is_base_of<optional_detail::optional_tag, BOOST_DEDUCED_TYPENAME boost::decay<U>::type>::value
                     || boost::is_same<BOOST_DEDUCED_TYPENAME boost::decay<U>::type, none_t>::value
                     || boost::is_same<BOOST_DEDUCED_TYPENAME boost::decay<U>::type, in_place_init_t>::value
                     || boost::is_same<BOOST_DEDUCED_TYPENAME boost::decay<U>::type, in_place_init_if_t>::value,
    boost::true_type, boost::false_type>::type
{};

template <typename T, typename U>
struct has_dedicated_constructor
  : boost::disjunction<is_optional_or_tag<U>, boost::is_same<T, BOOST_DEDUCED_TYPENAME boost::decay<U>::type> >
{};

template <typename U>
struct is_in_place_factory
  : boost::disjunction< boost::is_base_of<boost::in_place_factory_base, BOOST_DEDUCED_TYPENAME boost::decay<U>::type>,
                        boost::is_base_of<boost::typed_in_place_factory_base, BOOST_DEDUCED_TYPENAME boost::decay<U>::type> >
{};

#if !defined(BOOST_OPTIONAL_DETAIL_NO_IS_CONSTRUCTIBLE_TRAIT)

template <typename T, typename U>
struct is_factory_or_constructible_to_T
  : boost::disjunction< is_in_place_factory<U>, boost::is_constructible<T, U&&> >
{};

template <typename T, typename U>
struct is_optional_constructible : boost::is_constructible<T, U>
{};

#else

template <typename, typename>
struct is_factory_or_constructible_to_T : boost::true_type
{};

template <typename T, typename U>
struct is_optional_constructible : boost::true_type
{};

#endif // is_convertible condition

#if !defined(BOOST_NO_CXX11_DECLTYPE) && !BOOST_WORKAROUND(BOOST_MSVC, < 1800)
// for is_assignable

// On some initial rvalue reference implementations GCC does it in a strange way,
// preferring perfect-forwarding constructor to implicit copy constructor.

template <typename T, typename U>
struct is_opt_assignable
  : boost::conjunction<boost::is_convertible<U&&, T>, boost::is_assignable<T&, U&&> >
{};

#else

template <typename T, typename U>
struct is_opt_assignable : boost::is_convertible<U, T>
{};

#endif

template <typename T, typename U>
struct is_factory_or_opt_assignable_to_T
  : boost::disjunction< is_in_place_factory<U>, is_opt_assignable<T, U> >
{};

template <typename T, typename U, bool = has_dedicated_constructor<T, U>::value>
struct is_optional_val_init_candidate
  : boost::false_type
{};

template <typename T, typename U>
struct is_optional_val_init_candidate<T, U, false>
  : is_factory_or_constructible_to_T<T, U>
{};

template <typename T, typename U, bool = has_dedicated_constructor<T, U>::value>
struct is_optional_val_assign_candidate
  : boost::false_type
{};

template <typename T, typename U>
struct is_optional_val_assign_candidate<T, U, false>
  : is_factory_or_opt_assignable_to_T<T, U>
{};

} // namespace optional_detail

namespace optional_config {

template <typename T>
struct optional_uses_direct_storage_for
  : boost::conditional<(boost::is_scalar<T>::value && !boost::is_const<T>::value && !boost::is_volatile<T>::value)
                      , boost::true_type, boost::false_type>::type
{};

} // namespace optional_config


#ifndef BOOST_OPTIONAL_DETAIL_NO_DIRECT_STORAGE_SPEC
#  define BOOST_OPTIONAL_BASE_TYPE(T) boost::conditional< optional_config::optional_uses_direct_storage_for<T>::value, \
                                      optional_detail::tc_optional_base<T>, \
                                      optional_detail::optional_base<T> \
                                      >::type
#else
#  define BOOST_OPTIONAL_BASE_TYPE(T) optional_detail::optional_base<T>
#endif

template<class T>
class optional
  : public BOOST_OPTIONAL_BASE_TYPE(T)
{
    typedef typename BOOST_OPTIONAL_BASE_TYPE(T) base ;

  public :

    typedef optional<T> this_type ;

    typedef BOOST_DEDUCED_TYPENAME base::value_type           value_type ;
    typedef BOOST_DEDUCED_TYPENAME base::reference_type       reference_type ;
    typedef BOOST_DEDUCED_TYPENAME base::reference_const_type reference_const_type ;
    typedef BOOST_DEDUCED_TYPENAME base::rval_reference_type  rval_reference_type ;
    typedef BOOST_DEDUCED_TYPENAME base::reference_type_of_temporary_wrapper reference_type_of_temporary_wrapper ;
    typedef BOOST_DEDUCED_TYPENAME base::pointer_type         pointer_type ;
    typedef BOOST_DEDUCED_TYPENAME base::pointer_const_type   pointer_const_type ;
    typedef BOOST_DEDUCED_TYPENAME base::argument_type        argument_type ;

    // Creates an optional<T> uninitialized.
    // No-throw
    optional() BOOST_NOEXCEPT : base() {}

    // Creates an optional<T> uninitialized.
    // No-throw
    optional( none_t none_ ) BOOST_NOEXCEPT : base(none_) {}

    // Creates an optional<T> initialized with 'val'.
    // Can throw if T::T(T const&) does
    optional ( argument_type val ) : base(optional_detail::init_value_tag(), val) {}

    // Creates an optional<T> initialized with 'move(val)'.
    // Can throw if T::T(T &&) does
    optional ( rval_reference_type val ) : base(optional_detail::init_value_tag(), optional_detail::forward<T>(val))
      {}

    // Creates an optional<T> initialized with 'val' IFF cond is true, otherwise creates an uninitialized optional.
    // Can throw if T::T(T const&) does
    optional ( bool cond, argument_type val ) : base(cond,val) {}

    /// Creates an optional<T> initialized with 'val' IFF cond is true, otherwise creates an uninitialized optional.
    // Can throw if T::T(T &&) does
    optional ( bool cond, rval_reference_type val ) : base( cond, optional_detail::forward<T>(val) )
      {}

    // NOTE: MSVC needs templated versions first

    // Creates a deep copy of another convertible optional<U>
    // Requires a valid conversion from U to T.
    // Can throw if T::T(U const&) does
    template<class U>
    explicit optional ( optional<U> const& rhs
#ifndef BOOST_OPTIONAL_DETAIL_NO_SFINAE_FRIENDLY_CONSTRUCTORS
                        ,BOOST_DEDUCED_TYPENAME boost::enable_if< optional_detail::is_optional_constructible<T, U const&>, bool>::type = true
#endif
                      )
      :
      base()
    {
      if ( rhs.is_initialized() )
        this->construct(rhs.get());
    }

    // Creates a deep move of another convertible optional<U>
    // Requires a valid conversion from U to T.
    // Can throw if T::T(U&&) does
    template<class U>
    explicit optional ( optional<U> && rhs
#ifndef BOOST_OPTIONAL_DETAIL_NO_SFINAE_FRIENDLY_CONSTRUCTORS
                        ,BOOST_DEDUCED_TYPENAME boost::enable_if< optional_detail::is_optional_constructible<T, U>, bool>::type = true
#endif
                      )
      :
      base()
    {
      if ( rhs.is_initialized() )
        this->construct( optional_detail::move(rhs.get()) );
    }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
    // Creates an optional<T> with an expression which can be either
    //  (a) An instance of InPlaceFactory (i.e. in_place(a,b,...,n);
    //  (b) An instance of TypedInPlaceFactory ( i.e. in_place<T>(a,b,...,n);
    //  (c) Any expression implicitly convertible to the single type
    //      of a one-argument T's constructor.
    //  (d*) Weak compilers (BCB) might also resolved Expr as optional<T> and optional<U>
    //       even though explicit overloads are present for these.
    // Depending on the above some T ctor is called.
    // Can throw if the resolved T ctor throws.


  template<class Expr>
  explicit optional ( Expr&& expr,
                      BOOST_DEDUCED_TYPENAME boost::enable_if< optional_detail::is_optional_val_init_candidate<T, Expr>, bool>::type = true
  )
    : base(optional_detail::forward<Expr>(expr),boost::addressof(expr))
    {}

#endif // !defined BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT

    // Creates a deep copy of another optional<T>
    // Can throw if T::T(T const&) does
#ifndef BOOST_OPTIONAL_DETAIL_NO_DEFAULTED_MOVE_FUNCTIONS
    optional ( optional const& ) = default;
#else
    optional ( optional const& rhs ) : base( static_cast<base const&>(rhs) ) {}
#endif

    // Creates a deep move of another optional<T>
    // Can throw if T::T(T&&) does
#ifndef BOOST_OPTIONAL_DETAIL_NO_DEFAULTED_MOVE_FUNCTIONS
    optional ( optional && ) = default;
#else
    optional ( optional && rhs )
      BOOST_NOEXCEPT_IF(::boost::is_nothrow_move_constructible<T>::value)
      : base( optional_detail::move(rhs) )
    {}
#endif


#if BOOST_WORKAROUND(_MSC_VER, <= 1600)
    //  On old MSVC compilers the implicitly declared dtor is not called
    ~optional() {}
#endif


#if !defined(BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT) && !defined(BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION)
    // Assigns from an expression. See corresponding constructor.
    // Basic Guarantee: If the resolved T ctor throws, this is left UNINITIALIZED

    template<class Expr>
    BOOST_DEDUCED_TYPENAME boost::enable_if<optional_detail::is_optional_val_assign_candidate<T, Expr>, optional&>::type
    operator= ( Expr&& expr )
      {
        this->assign_expr(optional_detail::forward<Expr>(expr),boost::addressof(expr));
        return *this ;
      }

#endif // !defined(BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT) && !defined(BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION)

    // Copy-assigns from another convertible optional<U> (converts && deep-copies the rhs value)
    // Requires a valid conversion from U to T.
    // Basic Guarantee: If T::T( U const& ) throws, this is left UNINITIALIZED
    template<class U>
    optional& operator= ( optional<U> const& rhs )
      {
        this->assign(rhs);
        return *this ;
      }

    // Move-assigns from another convertible optional<U> (converts && deep-moves the rhs value)
    // Requires a valid conversion from U to T.
    // Basic Guarantee: If T::T( U && ) throws, this is left UNINITIALIZED
    template<class U>
    optional& operator= ( optional<U> && rhs )
      {
        this->assign(optional_detail::move(rhs));
        return *this ;
      }

    // Assigns from another optional<T> (deep-copies the rhs value)
    // Basic Guarantee: If T::T( T const& ) throws, this is left UNINITIALIZED
    //  (NOTE: On BCB, this operator is not actually called and left is left UNMODIFIED in case of a throw)
#ifndef BOOST_OPTIONAL_DETAIL_NO_DEFAULTED_MOVE_FUNCTIONS
    optional& operator= ( optional const& rhs ) = default;
#else
    optional& operator= ( optional const& rhs )
      {
        this->assign( static_cast<base const&>(rhs) ) ;
        return *this ;
      }
#endif

    // Assigns from another optional<T> (deep-moves the rhs value)
#ifndef BOOST_OPTIONAL_DETAIL_NO_DEFAULTED_MOVE_FUNCTIONS
    optional& operator= ( optional && ) = default;
#else
    optional& operator= ( optional && rhs )
      BOOST_NOEXCEPT_IF(::boost::is_nothrow_move_constructible<T>::value && ::boost::is_nothrow_move_assignable<T>::value)
      {
        this->assign( static_cast<base &&>(rhs) ) ;
        return *this ;
      }
#endif

#ifndef BOOST_NO_CXX11_UNIFIED_INITIALIZATION_SYNTAX

    // Assigns from a T (deep-moves/copies the rhs value)
    template <typename T_>
    BOOST_DEDUCED_TYPENAME boost::enable_if<boost::is_same<T, BOOST_DEDUCED_TYPENAME boost::decay<T_>::type>, optional&>::type
    operator= ( T_&& val )
      {
        this->assign( optional_detail::forward<T_>(val) ) ;
        return *this ;
      }

#else

    // Assigns from a T (deep-copies the rhs value)
    // Basic Guarantee: If T::( T const& ) throws, this is left UNINITIALIZED
    optional& operator= ( argument_type val )
      {
        this->assign( val ) ;
        return *this ;
      }

    // Assigns from a T (deep-moves the rhs value)
    optional& operator= ( rval_reference_type val )
      {
        this->assign( optional_detail::move(val) ) ;
        return *this ;
      }

#endif // BOOST_NO_CXX11_UNIFIED_INITIALIZATION_SYNTAX

    // Assigns from a "none"
    // Which destroys the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    optional& operator= ( none_t none_ ) BOOST_NOEXCEPT
      {
        this->assign( none_ ) ;
        return *this ;
      }

    // Constructs in-place
    // upon exception *this is always uninitialized
    template<class... Args>
    void emplace ( Args&&... args )
    {
      this->emplace_assign( optional_detail::forward<Args>(args)... );
    }

    template<class... Args>
    explicit optional ( in_place_init_t, Args&&... args )
    : base( in_place_init, optional_detail::forward<Args>(args)... )
    {}

    template<class... Args>
    explicit optional ( in_place_init_if_t, bool cond, Args&&... args )
    : base( in_place_init_if, cond, optional_detail::forward<Args>(args)... )
    {}

    void swap( optional & arg )
      BOOST_NOEXCEPT_IF(::boost::is_nothrow_move_constructible<T>::value && ::boost::is_nothrow_move_assignable<T>::value)
      {
        // allow for Koenig lookup
        boost::core::invoke_swap(*this, arg);
      }


    // Returns a reference to the value if this is initialized, otherwise,
    // the behaviour is UNDEFINED
    // No-throw
    reference_const_type get() const { BOOST_ASSERT(this->is_initialized()) ; return this->get_impl(); }
    reference_type       get()       { BOOST_ASSERT(this->is_initialized()) ; return this->get_impl(); }

    // Returns a copy of the value if this is initialized, 'v' otherwise
    reference_const_type get_value_or ( reference_const_type v ) const { return this->is_initialized() ? get() : v ; }
    reference_type       get_value_or ( reference_type       v )       { return this->is_initialized() ? get() : v ; }

    // Returns a pointer to the value if this is initialized, otherwise,
    // the behaviour is UNDEFINED
    // No-throw
    pointer_const_type operator->() const { BOOST_ASSERT(this->is_initialized()) ; return this->get_ptr_impl() ; }
    pointer_type       operator->()       { BOOST_ASSERT(this->is_initialized()) ; return this->get_ptr_impl() ; }

    // Returns a reference to the value if this is initialized, otherwise,
    // the behaviour is UNDEFINED
    // No-throw
    reference_const_type operator *() BOOST_OPTIONAL_CONST_REF_QUAL { return this->get() ; }
    reference_type       operator *() BOOST_OPTIONAL_REF_QUAL       { return this->get() ; }

#ifndef BOOST_NO_CXX11_REF_QUALIFIERS
    reference_type_of_temporary_wrapper operator *() && { return optional_detail::move(this->get()) ; }
#endif

    reference_const_type value() BOOST_OPTIONAL_CONST_REF_QUAL
      {
        if (this->is_initialized())
          return this->get() ;
        else
          throw_exception(bad_optional_access());
      }

    reference_type value() BOOST_OPTIONAL_REF_QUAL
      {
        if (this->is_initialized())
          return this->get() ;
        else
          throw_exception(bad_optional_access());
      }

    template <class U>
    value_type value_or ( U&& v ) BOOST_OPTIONAL_CONST_REF_QUAL
      {
        if (this->is_initialized())
          return get();
        else
          return optional_detail::forward<U>(v);
      }

    template <typename F>
    value_type value_or_eval ( F f ) BOOST_OPTIONAL_CONST_REF_QUAL
      {
        if (this->is_initialized())
          return get();
        else
          return f();
      }

#ifndef BOOST_NO_CXX11_REF_QUALIFIERS
    reference_type_of_temporary_wrapper value() &&
      {
        if (this->is_initialized())
          return optional_detail::move(this->get()) ;
        else
          throw_exception(bad_optional_access());
      }

    template <class U>
    value_type value_or ( U&& v ) &&
      {
        if (this->is_initialized())
          return optional_detail::move(get());
        else
          return optional_detail::forward<U>(v);
      }

    template <typename F>
    value_type value_or_eval ( F f ) &&
      {
        if (this->is_initialized())
          return optional_detail::move(get());
        else
          return f();
      }
#endif

// Monadic interface

    template <typename F>
    optional<typename optional_detail::result_of<F, reference_type>::type> map(F f) BOOST_OPTIONAL_REF_QUAL
      {
        if (this->has_value())
          return f(get());
        else
          return none;
      }

    template <typename F>
    optional<typename optional_detail::result_of<F, reference_const_type>::type> map(F f) BOOST_OPTIONAL_CONST_REF_QUAL
      {
        if (this->has_value())
          return f(get());
        else
          return none;
      }

#ifndef BOOST_NO_CXX11_REF_QUALIFIERS
    template <typename F>
    optional<typename optional_detail::result_of<F, reference_type_of_temporary_wrapper>::type> map(F f) &&
      {
        if (this->has_value())
          return f(optional_detail::move(this->get()));
        else
          return none;
      }
#endif

    template <typename F>
    optional<typename optional_detail::result_value_type<F, reference_type>::type>
    flat_map(F f) BOOST_OPTIONAL_REF_QUAL
      {
        if (this->has_value())
          return f(get());
        else
          return none;
      }

    template <typename F>
    optional<typename optional_detail::result_value_type<F, reference_const_type>::type>
    flat_map(F f) BOOST_OPTIONAL_CONST_REF_QUAL
      {
        if (this->has_value())
          return f(get());
        else
          return none;
      }

#ifndef BOOST_NO_CXX11_REF_QUALIFIERS
    template <typename F>
    optional<typename optional_detail::result_value_type<F, reference_type_of_temporary_wrapper>::type>
    flat_map(F f) &&
      {
        if (this->has_value())
          return f(optional_detail::move(get()));
        else
          return none;
      }
#endif

    bool has_value() const BOOST_NOEXCEPT { return this->is_initialized() ; }

    explicit operator bool() const BOOST_NOEXCEPT { return this->has_value() ; }
} ;


template<class T>
class optional<T&&>
{
  static_assert(sizeof(T) == 0, "Optional rvalue references are illegal.");
} ;

} // namespace boost

#ifndef BOOST_OPTIONAL_CONFIG_DONT_SPECIALIZE_OPTIONAL_REFS
# include <boost/optional/detail/optional_reference_spec.hpp>
#endif

namespace boost {


template<class T>
inline
optional<BOOST_DEDUCED_TYPENAME boost::decay<T>::type> make_optional ( T && v  )
{
  return optional<BOOST_DEDUCED_TYPENAME boost::decay<T>::type>(optional_detail::forward<T>(v));
}

// Returns optional<T>(cond,v)
template<class T>
inline
optional<BOOST_DEDUCED_TYPENAME boost::decay<T>::type> make_optional ( bool cond, T && v )
{
  return optional<BOOST_DEDUCED_TYPENAME boost::decay<T>::type>(cond,optional_detail::forward<T>(v));
}


// Returns a reference to the value if this is initialized, otherwise, the behaviour is UNDEFINED.
// No-throw
template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::reference_const_type
get ( optional<T> const& opt )
{
  return opt.get() ;
}

template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::reference_type
get ( optional<T>& opt )
{
  return opt.get() ;
}

// Returns a pointer to the value if this is initialized, otherwise, returns NULL.
// No-throw
template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::pointer_const_type
get ( optional<T> const* opt )
{
  return opt->get_ptr() ;
}

template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::pointer_type
get ( optional<T>* opt )
{
  return opt->get_ptr() ;
}

// Returns a reference to the value if this is initialized, otherwise, the behaviour is UNDEFINED.
// No-throw
template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::reference_const_type
get_optional_value_or ( optional<T> const& opt, BOOST_DEDUCED_TYPENAME optional<T>::reference_const_type v )
{
  return opt.get_value_or(v) ;
}

template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::reference_type
get_optional_value_or ( optional<T>& opt, BOOST_DEDUCED_TYPENAME optional<T>::reference_type v )
{
  return opt.get_value_or(v) ;
}

// Returns a pointer to the value if this is initialized, otherwise, returns NULL.
// No-throw
template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::pointer_const_type
get_pointer ( optional<T> const& opt )
{
  return opt.get_ptr() ;
}

template<class T>
inline
BOOST_DEDUCED_TYPENAME optional<T>::pointer_type
get_pointer ( optional<T>& opt )
{
  return opt.get_ptr() ;
}

} // namespace boost

#ifndef BOOST_NO_IOSTREAM
namespace boost {

// The following declaration prevents a bug where operator safe-bool is used upon streaming optional object if you forget the IO header.
template<class CharType, class CharTrait>
std::basic_ostream<CharType, CharTrait>&
operator<<(std::basic_ostream<CharType, CharTrait>& os, optional_detail::optional_tag const&)
{
  static_assert(sizeof(CharType) == 0, "If you want to output boost::optional, include header <boost/optional/optional_io.hpp>");
  return os;
}

} // namespace boost
#endif // BOOST_NO_IOSTREAM

#include <boost/optional/detail/optional_relops.hpp>
#include <boost/optional/detail/optional_swap.hpp>

#endif // header guard
