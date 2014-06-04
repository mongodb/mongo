// Copyright (C) 2003, 2008 Fernando Luis Cacciola Carballal.
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
//
#ifndef BOOST_OPTIONAL_OPTIONAL_FLC_19NOV2002_HPP
#define BOOST_OPTIONAL_OPTIONAL_FLC_19NOV2002_HPP

#include <new>
#include <algorithm>

#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/type.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/type_traits/has_nothrow_constructor.hpp>
#include <boost/type_traits/type_with_alignment.hpp>
#include <boost/type_traits/remove_reference.hpp>
#include <boost/type_traits/is_reference.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/not.hpp>
#include <boost/detail/reference_content.hpp>
#include <boost/none.hpp>
#include <boost/utility/swap.hpp>
#include <boost/utility/addressof.hpp>
#include <boost/utility/compare_pointees.hpp>
#include <boost/utility/in_place_factory.hpp>

#include <boost/optional/optional_fwd.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, == 1200)
// VC6.0 has the following bug:
//   When a templated assignment operator exist, an implicit conversion
//   constructing an optional<T> is used when assigment of the form:
//     optional<T> opt ; opt = T(...);
//   is compiled.
//   However, optional's ctor is _explicit_ and the assignemt shouldn't compile.
//   Therefore, for VC6.0 templated assignment is disabled.
//
#define BOOST_OPTIONAL_NO_CONVERTING_ASSIGNMENT
#endif

#if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
// VC7.0 has the following bug:
//   When both a non-template and a template copy-ctor exist
//   and the templated version is made 'explicit', the explicit is also
//   given to the non-templated version, making the class non-implicitely-copyable.
//
#define BOOST_OPTIONAL_NO_CONVERTING_COPY_CTOR
#endif

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300) || BOOST_WORKAROUND(BOOST_INTEL_CXX_VERSION,<=700)
// AFAICT only VC7.1 correctly resolves the overload set
// that includes the in-place factory taking functions,
// so for the other VC versions, in-place factory support
// is disabled
#define BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
#endif

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
// BCB (5.5.1) cannot parse the nested template struct in an inplace factory.
#define BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
#endif

#if !defined(BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT) \
    && BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x581) )
// BCB (up to 5.64) has the following bug:
//   If there is a member function/operator template of the form
//     template<class Expr> mfunc( Expr expr ) ;
//   some calls are resolved to this even if there are other better matches.
//   The effect of this bug is that calls to converting ctors and assignments
//   are incrorrectly sink to this general catch-all member function template as shown above.
#define BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION
#endif

#if defined(__GNUC__) && (__GNUC__ * 100 + __GNUC_MINOR__) > 302 \
    && !defined(__INTEL_COMPILER)
// GCC since 3.3 has may_alias attribute that helps to alleviate optimizer issues with
// regard to violation of the strict aliasing rules. The optional< T > storage type is marked
// with this attribute in order to let the compiler know that it will alias objects of type T
// and silence compilation warnings.
#define BOOST_OPTIONAL_DETAIL_USE_ATTRIBUTE_MAY_ALIAS
#endif

// Daniel Wallin discovered that bind/apply.hpp badly interacts with the apply<>
// member template of a factory as used in the optional<> implementation.
// He proposed this simple fix which is to move the call to apply<> outside
// namespace boost.
namespace boost_optional_detail
{
  template <class T, class Factory>
  inline void construct(Factory const& factory, void* address)
  {
    factory.BOOST_NESTED_TEMPLATE apply<T>(address);
  }
}


namespace boost {

class in_place_factory_base ;
class typed_in_place_factory_base ;

// This forward is needed to refer to namespace scope swap from the member swap
template<class T> void swap ( optional<T>& x, optional<T>& y );

namespace optional_detail {

// This local class is used instead of that in "aligned_storage.hpp"
// because I've found the 'official' class to ICE BCB5.5
// when some types are used with optional<>
// (due to sizeof() passed down as a non-type template parameter)
template <class T>
class aligned_storage
{
    // Borland ICEs if unnamed unions are used for this!
    union
    // This works around GCC warnings about breaking strict aliasing rules when casting storage address to T*
#if defined(BOOST_OPTIONAL_DETAIL_USE_ATTRIBUTE_MAY_ALIAS)
    __attribute__((may_alias))
#endif
    dummy_u
    {
        char data[ sizeof(T) ];
        BOOST_DEDUCED_TYPENAME type_with_alignment<
          ::boost::alignment_of<T>::value >::type aligner_;
    } dummy_ ;

  public:

#if defined(BOOST_OPTIONAL_DETAIL_USE_ATTRIBUTE_MAY_ALIAS)
    void const* address() const { return &dummy_; }
    void      * address()       { return &dummy_; }
#else
    void const* address() const { return dummy_.data; }
    void      * address()       { return dummy_.data; }
#endif
} ;

template<class T>
struct types_when_isnt_ref
{
  typedef T const& reference_const_type ;
  typedef T &      reference_type ;
  typedef T const* pointer_const_type ;
  typedef T *      pointer_type ;
  typedef T const& argument_type ;
} ;
template<class T>
struct types_when_is_ref
{
  typedef BOOST_DEDUCED_TYPENAME remove_reference<T>::type raw_type ;

  typedef raw_type& reference_const_type ;
  typedef raw_type& reference_type ;
  typedef raw_type* pointer_const_type ;
  typedef raw_type* pointer_type ;
  typedef raw_type& argument_type ;
} ;

struct optional_tag {} ;

template<class T>
class optional_base : public optional_tag
{
  private :

    typedef
#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))
    BOOST_DEDUCED_TYPENAME
#endif
    ::boost::detail::make_reference_content<T>::type internal_type ;

    typedef aligned_storage<internal_type> storage_type ;

    typedef types_when_isnt_ref<T> types_when_not_ref ;
    typedef types_when_is_ref<T>   types_when_ref   ;

    typedef optional_base<T> this_type ;

  protected :

    typedef T value_type ;

    typedef mpl::true_  is_reference_tag ;
    typedef mpl::false_ is_not_reference_tag ;

    typedef BOOST_DEDUCED_TYPENAME is_reference<T>::type is_reference_predicate ;

  public:
    typedef BOOST_DEDUCED_TYPENAME mpl::if_<is_reference_predicate,types_when_ref,types_when_not_ref>::type types ;

  protected:
    typedef bool (this_type::*unspecified_bool_type)() const;

    typedef BOOST_DEDUCED_TYPENAME types::reference_type       reference_type ;
    typedef BOOST_DEDUCED_TYPENAME types::reference_const_type reference_const_type ;
    typedef BOOST_DEDUCED_TYPENAME types::pointer_type         pointer_type ;
    typedef BOOST_DEDUCED_TYPENAME types::pointer_const_type   pointer_const_type ;
    typedef BOOST_DEDUCED_TYPENAME types::argument_type        argument_type ;

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
    optional_base ( argument_type val )
      :
      m_initialized(false)
    {
      construct(val);
    }

    // Creates an optional<T> initialized with 'val' IFF cond is true, otherwise creates an uninitialzed optional<T>.
    // Can throw if T::T(T const&) does
    optional_base ( bool cond, argument_type val )
      :
      m_initialized(false)
    {
      if ( cond )
        construct(val);
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


    // This is used for both converting and in-place constructions.
    // Derived classes use the 'tag' to select the appropriate
    // implementation (the correct 'construct()' overload)
    template<class Expr>
    explicit optional_base ( Expr const& expr, Expr const* tag )
      :
      m_initialized(false)
    {
      construct(expr,tag);
    }



    // No-throw (assuming T::~T() doesn't)
    ~optional_base() { destroy() ; }

    // Assigns from another optional<T> (deep-copies the rhs value)
    void assign ( optional_base const& rhs )
    {
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
             assign_value(rhs.get_impl(), is_reference_predicate() );
        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
          construct(rhs.get_impl());
      }
    }

    // Assigns from another _convertible_ optional<U> (deep-copies the rhs value)
    template<class U>
    void assign ( optional<U> const& rhs )
    {
      if (is_initialized())
      {
        if ( rhs.is_initialized() )
             assign_value(static_cast<value_type>(rhs.get()), is_reference_predicate() );
        else destroy();
      }
      else
      {
        if ( rhs.is_initialized() )
          construct(static_cast<value_type>(rhs.get()));
      }
    }

    // Assigns from a T (deep-copies the rhs value)
    void assign ( argument_type val )
    {
      if (is_initialized())
           assign_value(val, is_reference_predicate() );
      else construct(val);
    }

    // Assigns from "none", destroying the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    void assign ( none_t ) { destroy(); }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
    template<class Expr>
    void assign_expr ( Expr const& expr, Expr const* tag )
      {
        if (is_initialized())
             assign_expr_to_initialized(expr,tag);
        else construct(expr,tag);
      }
#endif

  public :

    // Destroys the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    void reset() { destroy(); }

    // Replaces the current value -if any- with 'val'
    void reset ( argument_type val ) { assign(val); }

    // Returns a pointer to the value if this is initialized, otherwise,
    // returns NULL.
    // No-throw
    pointer_const_type get_ptr() const { return m_initialized ? get_ptr_impl() : 0 ; }
    pointer_type       get_ptr()       { return m_initialized ? get_ptr_impl() : 0 ; }

    bool is_initialized() const { return m_initialized ; }

  protected :

    void construct ( argument_type val )
     {
       new (m_storage.address()) internal_type(val) ;
       m_initialized = true ;
     }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
    // Constructs in-place using the given factory
    template<class Expr>
    void construct ( Expr const& factory, in_place_factory_base const* )
     {
       BOOST_STATIC_ASSERT ( ::boost::mpl::not_<is_reference_predicate>::value ) ;
       boost_optional_detail::construct<value_type>(factory, m_storage.address());
       m_initialized = true ;
     }

    // Constructs in-place using the given typed factory
    template<class Expr>
    void construct ( Expr const& factory, typed_in_place_factory_base const* )
     {
       BOOST_STATIC_ASSERT ( ::boost::mpl::not_<is_reference_predicate>::value ) ;
       factory.apply(m_storage.address()) ;
       m_initialized = true ;
     }

    template<class Expr>
    void assign_expr_to_initialized ( Expr const& factory, in_place_factory_base const* tag )
     {
       destroy();
       construct(factory,tag);
     }

    // Constructs in-place using the given typed factory
    template<class Expr>
    void assign_expr_to_initialized ( Expr const& factory, typed_in_place_factory_base const* tag )
     {
       destroy();
       construct(factory,tag);
     }
#endif

    // Constructs using any expression implicitely convertible to the single argument
    // of a one-argument T constructor.
    // Converting constructions of optional<T> from optional<U> uses this function with
    // 'Expr' being of type 'U' and relying on a converting constructor of T from U.
    template<class Expr>
    void construct ( Expr const& expr, void const* )
     {
       new (m_storage.address()) internal_type(expr) ;
       m_initialized = true ;
     }

    // Assigns using a form any expression implicitely convertible to the single argument
    // of a T's assignment operator.
    // Converting assignments of optional<T> from optional<U> uses this function with
    // 'Expr' being of type 'U' and relying on a converting assignment of T from U.
    template<class Expr>
    void assign_expr_to_initialized ( Expr const& expr, void const* )
     {
       assign_value(expr, is_reference_predicate());
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
    // For VC<=70 compilers this workaround dosen't work becasue the comnpiler issues and error
    // instead of choosing the wrong overload
    //
    // Notice that 'Expr' will be optional<T> or optional<U> (but not optional_base<..>)
    template<class Expr>
    void construct ( Expr const& expr, optional_tag const* )
     {
       if ( expr.is_initialized() )
       {
         // An exception can be thrown here.
         // It it happens, THIS will be left uninitialized.
         new (m_storage.address()) internal_type(expr.get()) ;
         m_initialized = true ;
       }
     }
#endif

    void assign_value ( argument_type val, is_not_reference_tag ) { get_impl() = val; }
    void assign_value ( argument_type val, is_reference_tag     ) { construct(val); }

    void destroy()
    {
      if ( m_initialized )
        destroy_impl(is_reference_predicate()) ;
    }

    unspecified_bool_type safe_bool() const { return m_initialized ? &this_type::is_initialized : 0 ; }

    reference_const_type get_impl() const { return dereference(get_object(), is_reference_predicate() ) ; }
    reference_type       get_impl()       { return dereference(get_object(), is_reference_predicate() ) ; }

    pointer_const_type get_ptr_impl() const { return cast_ptr(get_object(), is_reference_predicate() ) ; }
    pointer_type       get_ptr_impl()       { return cast_ptr(get_object(), is_reference_predicate() ) ; }

  private :

    // internal_type can be either T or reference_content<T>
#if defined(BOOST_OPTIONAL_DETAIL_USE_ATTRIBUTE_MAY_ALIAS)
    // This workaround is supposed to silence GCC warnings about broken strict aliasing rules
    internal_type const* get_object() const
    {
        union { void const* ap_pvoid; internal_type const* as_ptype; } caster = { m_storage.address() };
        return caster.as_ptype;
    }
    internal_type *      get_object()
    {
        union { void* ap_pvoid; internal_type* as_ptype; } caster = { m_storage.address() };
        return caster.as_ptype;
    }
#else
    internal_type const* get_object() const { return static_cast<internal_type const*>(m_storage.address()); }
    internal_type *      get_object()       { return static_cast<internal_type *>     (m_storage.address()); }
#endif

    // reference_content<T> lacks an implicit conversion to T&, so the following is needed to obtain a proper reference.
    reference_const_type dereference( internal_type const* p, is_not_reference_tag ) const { return *p ; }
    reference_type       dereference( internal_type*       p, is_not_reference_tag )       { return *p ; }
    reference_const_type dereference( internal_type const* p, is_reference_tag     ) const { return p->get() ; }
    reference_type       dereference( internal_type*       p, is_reference_tag     )       { return p->get() ; }

#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x581))
    void destroy_impl ( is_not_reference_tag ) { get_ptr_impl()->internal_type::~internal_type() ; m_initialized = false ; }
#else
    void destroy_impl ( is_not_reference_tag ) { get_ptr_impl()->T::~T() ; m_initialized = false ; }
#endif

    void destroy_impl ( is_reference_tag     ) { m_initialized = false ; }

    // If T is of reference type, trying to get a pointer to the held value must result in a compile-time error.
    // Decent compilers should disallow conversions from reference_content<T>* to T*, but just in case,
    // the following olverloads are used to filter out the case and guarantee an error in case of T being a reference.
    pointer_const_type cast_ptr( internal_type const* p, is_not_reference_tag ) const { return p ; }
    pointer_type       cast_ptr( internal_type *      p, is_not_reference_tag )       { return p ; }
    pointer_const_type cast_ptr( internal_type const* p, is_reference_tag     ) const { return &p->get() ; }
    pointer_type       cast_ptr( internal_type *      p, is_reference_tag     )       { return &p->get() ; }

    bool m_initialized ;
    storage_type m_storage ;
} ;

} // namespace optional_detail

template<class T>
class optional : public optional_detail::optional_base<T>
{
    typedef optional_detail::optional_base<T> base ;

    typedef BOOST_DEDUCED_TYPENAME base::unspecified_bool_type  unspecified_bool_type ;

  public :

    typedef optional<T> this_type ;

    typedef BOOST_DEDUCED_TYPENAME base::value_type           value_type ;
    typedef BOOST_DEDUCED_TYPENAME base::reference_type       reference_type ;
    typedef BOOST_DEDUCED_TYPENAME base::reference_const_type reference_const_type ;
    typedef BOOST_DEDUCED_TYPENAME base::pointer_type         pointer_type ;
    typedef BOOST_DEDUCED_TYPENAME base::pointer_const_type   pointer_const_type ;
    typedef BOOST_DEDUCED_TYPENAME base::argument_type        argument_type ;

    // Creates an optional<T> uninitialized.
    // No-throw
    optional() : base() {}

    // Creates an optional<T> uninitialized.
    // No-throw
    optional( none_t none_ ) : base(none_) {}

    // Creates an optional<T> initialized with 'val'.
    // Can throw if T::T(T const&) does
    optional ( argument_type val ) : base(val) {}

    // Creates an optional<T> initialized with 'val' IFF cond is true, otherwise creates an uninitialized optional.
    // Can throw if T::T(T const&) does
    optional ( bool cond, argument_type val ) : base(cond,val) {}

#ifndef BOOST_OPTIONAL_NO_CONVERTING_COPY_CTOR
    // NOTE: MSVC needs templated versions first

    // Creates a deep copy of another convertible optional<U>
    // Requires a valid conversion from U to T.
    // Can throw if T::T(U const&) does
    template<class U>
    explicit optional ( optional<U> const& rhs )
      :
      base()
    {
      if ( rhs.is_initialized() )
        this->construct(rhs.get());
    }
#endif

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT
    // Creates an optional<T> with an expression which can be either
    //  (a) An instance of InPlaceFactory (i.e. in_place(a,b,...,n);
    //  (b) An instance of TypedInPlaceFactory ( i.e. in_place<T>(a,b,...,n);
    //  (c) Any expression implicitely convertible to the single type
    //      of a one-argument T's constructor.
    //  (d*) Weak compilers (BCB) might also resolved Expr as optional<T> and optional<U>
    //       even though explicit overloads are present for these.
    // Depending on the above some T ctor is called.
    // Can throw is the resolved T ctor throws.
    template<class Expr>
    explicit optional ( Expr const& expr ) : base(expr,boost::addressof(expr)) {}
#endif

    // Creates a deep copy of another optional<T>
    // Can throw if T::T(T const&) does
    optional ( optional const& rhs ) : base( static_cast<base const&>(rhs) ) {}

   // No-throw (assuming T::~T() doesn't)
    ~optional() {}

#if !defined(BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT) && !defined(BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION)
    // Assigns from an expression. See corresponding constructor.
    // Basic Guarantee: If the resolved T ctor throws, this is left UNINITIALIZED
    template<class Expr>
    optional& operator= ( Expr const& expr )
      {
        this->assign_expr(expr,boost::addressof(expr));
        return *this ;
      }
#endif


#ifndef BOOST_OPTIONAL_NO_CONVERTING_ASSIGNMENT
    // Assigns from another convertible optional<U> (converts && deep-copies the rhs value)
    // Requires a valid conversion from U to T.
    // Basic Guarantee: If T::T( U const& ) throws, this is left UNINITIALIZED
    template<class U>
    optional& operator= ( optional<U> const& rhs )
      {
        this->assign(rhs);
        return *this ;
      }
#endif

    // Assigns from another optional<T> (deep-copies the rhs value)
    // Basic Guarantee: If T::T( T const& ) throws, this is left UNINITIALIZED
    //  (NOTE: On BCB, this operator is not actually called and left is left UNMODIFIED in case of a throw)
    optional& operator= ( optional const& rhs )
      {
        this->assign( static_cast<base const&>(rhs) ) ;
        return *this ;
      }

    // Assigns from a T (deep-copies the rhs value)
    // Basic Guarantee: If T::( T const& ) throws, this is left UNINITIALIZED
    optional& operator= ( argument_type val )
      {
        this->assign( val ) ;
        return *this ;
      }

    // Assigns from a "none"
    // Which destroys the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    optional& operator= ( none_t none_ )
      {
        this->assign( none_ ) ;
        return *this ;
      }

    void swap( optional & arg )
      {
        // allow for Koenig lookup
        using boost::swap;
        swap(*this, arg);
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
    reference_const_type operator *() const { return this->get() ; }
    reference_type       operator *()       { return this->get() ; }

    // implicit conversion to "bool"
    // No-throw
    operator unspecified_bool_type() const { return this->safe_bool() ; }

    // This is provided for those compilers which don't like the conversion to bool
    // on some contexts.
    bool operator!() const { return !this->is_initialized() ; }
} ;

// Returns optional<T>(v)
template<class T>
inline
optional<T> make_optional ( T const& v  )
{
  return optional<T>(v);
}

// Returns optional<T>(cond,v)
template<class T>
inline
optional<T> make_optional ( bool cond, T const& v )
{
  return optional<T>(cond,v);
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

// optional's relational operators ( ==, !=, <, >, <=, >= ) have deep-semantics (compare values).
// WARNING: This is UNLIKE pointers. Use equal_pointees()/less_pointess() in generic code instead.


//
// optional<T> vs optional<T> cases
//

template<class T>
inline
bool operator == ( optional<T> const& x, optional<T> const& y )
{ return equal_pointees(x,y); }

template<class T>
inline
bool operator < ( optional<T> const& x, optional<T> const& y )
{ return less_pointees(x,y); }

template<class T>
inline
bool operator != ( optional<T> const& x, optional<T> const& y )
{ return !( x == y ) ; }

template<class T>
inline
bool operator > ( optional<T> const& x, optional<T> const& y )
{ return y < x ; }

template<class T>
inline
bool operator <= ( optional<T> const& x, optional<T> const& y )
{ return !( y < x ) ; }

template<class T>
inline
bool operator >= ( optional<T> const& x, optional<T> const& y )
{ return !( x < y ) ; }


//
// optional<T> vs T cases
//
template<class T>
inline
bool operator == ( optional<T> const& x, T const& y )
{ return equal_pointees(x, optional<T>(y)); }

template<class T>
inline
bool operator < ( optional<T> const& x, T const& y )
{ return less_pointees(x, optional<T>(y)); }

template<class T>
inline
bool operator != ( optional<T> const& x, T const& y )
{ return !( x == y ) ; }

template<class T>
inline
bool operator > ( optional<T> const& x, T const& y )
{ return y < x ; }

template<class T>
inline
bool operator <= ( optional<T> const& x, T const& y )
{ return !( y < x ) ; }

template<class T>
inline
bool operator >= ( optional<T> const& x, T const& y )
{ return !( x < y ) ; }

//
// T vs optional<T> cases
//

template<class T>
inline
bool operator == ( T const& x, optional<T> const& y )
{ return equal_pointees( optional<T>(x), y ); }

template<class T>
inline
bool operator < ( T const& x, optional<T> const& y )
{ return less_pointees( optional<T>(x), y ); }

template<class T>
inline
bool operator != ( T const& x, optional<T> const& y )
{ return !( x == y ) ; }

template<class T>
inline
bool operator > ( T const& x, optional<T> const& y )
{ return y < x ; }

template<class T>
inline
bool operator <= ( T const& x, optional<T> const& y )
{ return !( y < x ) ; }

template<class T>
inline
bool operator >= ( T const& x, optional<T> const& y )
{ return !( x < y ) ; }


//
// optional<T> vs none cases
//

template<class T>
inline
bool operator == ( optional<T> const& x, none_t )
{ return equal_pointees(x, optional<T>() ); }

template<class T>
inline
bool operator < ( optional<T> const& x, none_t )
{ return less_pointees(x,optional<T>() ); }

template<class T>
inline
bool operator != ( optional<T> const& x, none_t y )
{ return !( x == y ) ; }

template<class T>
inline
bool operator > ( optional<T> const& x, none_t y )
{ return y < x ; }

template<class T>
inline
bool operator <= ( optional<T> const& x, none_t y )
{ return !( y < x ) ; }

template<class T>
inline
bool operator >= ( optional<T> const& x, none_t y )
{ return !( x < y ) ; }

//
// none vs optional<T> cases
//

template<class T>
inline
bool operator == ( none_t , optional<T> const& y )
{ return equal_pointees(optional<T>() ,y); }

template<class T>
inline
bool operator < ( none_t , optional<T> const& y )
{ return less_pointees(optional<T>() ,y); }

template<class T>
inline
bool operator != ( none_t x, optional<T> const& y )
{ return !( x == y ) ; }

template<class T>
inline
bool operator > ( none_t x, optional<T> const& y )
{ return y < x ; }

template<class T>
inline
bool operator <= ( none_t x, optional<T> const& y )
{ return !( y < x ) ; }

template<class T>
inline
bool operator >= ( none_t x, optional<T> const& y )
{ return !( x < y ) ; }

namespace optional_detail {

template<bool use_default_constructor> struct swap_selector;

template<>
struct swap_selector<true>
{
    template<class T>
    static void optional_swap ( optional<T>& x, optional<T>& y )
    {
        const bool hasX = !!x;
        const bool hasY = !!y;

        if ( !hasX && !hasY )
            return;

        if( !hasX )
            x = boost::in_place();
        else if ( !hasY )
            y = boost::in_place();

        // Boost.Utility.Swap will take care of ADL and workarounds for broken compilers
        boost::swap(x.get(),y.get());

        if( !hasX )
            y = boost::none ;
        else if( !hasY )
            x = boost::none ;
    }
};

template<>
struct swap_selector<false>
{
    template<class T>
    static void optional_swap ( optional<T>& x, optional<T>& y )
    {
        const bool hasX = !!x;
        const bool hasY = !!y;

        if ( !hasX && hasY )
        {
            x = y.get();
            y = boost::none ;
        }
        else if ( hasX && !hasY )
        {
            y = x.get();
            x = boost::none ;
        }
        else if ( hasX && hasY )
        {
            // Boost.Utility.Swap will take care of ADL and workarounds for broken compilers
            boost::swap(x.get(),y.get());
        }
    }
};

} // namespace optional_detail

template<class T>
struct optional_swap_should_use_default_constructor : has_nothrow_default_constructor<T> {} ;

template<class T> inline void swap ( optional<T>& x, optional<T>& y )
{
    optional_detail::swap_selector<optional_swap_should_use_default_constructor<T>::value>::optional_swap(x, y);
}

} // namespace boost

#endif
