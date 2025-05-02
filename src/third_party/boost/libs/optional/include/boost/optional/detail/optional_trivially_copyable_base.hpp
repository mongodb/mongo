// Copyright (C) 2017 Andrzej Krzemienski.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  akrzemi1@gmail.com

// trivially-copyable version of the storage

template<class T>
class tc_optional_base : public optional_tag
{
  private :

    typedef tc_optional_base<T> this_type ;

  protected :

    typedef T value_type ;

  protected:
    typedef T &       reference_type ;
    typedef T const&  reference_const_type ;
    typedef T &&  rval_reference_type ;
    typedef T &&  reference_type_of_temporary_wrapper ;
    typedef T *         pointer_type ;
    typedef T const*    pointer_const_type ;
    typedef T const&    argument_type ;

    tc_optional_base()
      :
      m_initialized(false), m_storage() {}

    tc_optional_base ( none_t )
      :
      m_initialized(false), m_storage() {}

    tc_optional_base ( init_value_tag, argument_type val )
      :
      m_initialized(true), m_storage(val) {}

    tc_optional_base ( bool cond, argument_type val )
      :
      m_initialized(cond), m_storage(val) {}

    // tc_optional_base ( tc_optional_base const& ) = default;

    template<class Expr, class PtrExpr>
    explicit tc_optional_base ( Expr&& expr, PtrExpr const* tag )
      :
      m_initialized(false)
    {
      construct(optional_detail::forward<Expr>(expr),tag);
    }

    // tc_optional_base& operator= ( tc_optional_base const& ) = default;
    // ~tc_optional_base() = default;

    // Assigns from another optional<T> (deep-copies the rhs value)
    void assign ( tc_optional_base const& rhs )
    {
      *this = rhs;
    }

    // Assigns from another _convertible_ optional<U> (deep-copies the rhs value)
    template<class U>
    void assign ( optional<U> const& rhs )
    {
      if ( rhs.is_initialized() )
#ifndef BOOST_OPTIONAL_CONFIG_RESTORE_ASSIGNMENT_OF_NONCONVERTIBLE_TYPES
        m_storage = rhs.get();
#else
        m_storage = static_cast<value_type>(rhs.get());
#endif

      m_initialized = rhs.is_initialized();
    }

    // move-assigns from another _convertible_ optional<U> (deep-moves from the rhs value)
    template<class U>
    void assign ( optional<U>&& rhs )
    {
      typedef BOOST_DEDUCED_TYPENAME optional<U>::rval_reference_type ref_type;
      if ( rhs.is_initialized() )
        m_storage = static_cast<ref_type>(rhs.get());
      m_initialized = rhs.is_initialized();
    }

    void assign ( argument_type val )
    {
      construct(val);
    }

    void assign ( none_t ) { destroy(); }

#ifndef BOOST_OPTIONAL_NO_INPLACE_FACTORY_SUPPORT

    template<class Expr, class ExprPtr>
    void assign_expr ( Expr&& expr, ExprPtr const* tag )
    {
       construct(optional_detail::forward<Expr>(expr),tag);
    }

#endif

  public :

    // Destroys the current value, if any, leaving this UNINITIALIZED
    // No-throw (assuming T::~T() doesn't)
    void reset() BOOST_NOEXCEPT { destroy(); }

    // **DEPRECATED** Replaces the current value -if any- with 'val'
    void reset ( argument_type val ) BOOST_NOEXCEPT { assign(val); }

    // Returns a pointer to the value if this is initialized, otherwise,
    // returns NULL.
    // No-throw
    pointer_const_type get_ptr() const { return m_initialized ? get_ptr_impl() : 0 ; }
    pointer_type       get_ptr()       { return m_initialized ? get_ptr_impl() : 0 ; }

    bool is_initialized() const { return m_initialized ; }

  protected :

    void construct ( argument_type val )
     {
       m_storage = val ;
       m_initialized = true ;
     }


    // Constructs in-place
    // upon exception *this is always uninitialized
    template<class... Args>
    void construct ( in_place_init_t, Args&&... args )
    {
      m_storage = value_type( optional_detail::forward<Args>(args)... ) ;
      m_initialized = true ;
    }

    template<class... Args>
    void emplace_assign ( Args&&... args )
    {
      construct(in_place_init, optional_detail::forward<Args>(args)...);
    }

    template<class... Args>
    explicit tc_optional_base ( in_place_init_t, Args&&... args )
      :
      m_initialized(false)
    {
      construct(in_place_init, optional_detail::forward<Args>(args)...);
    }

    template<class... Args>
    explicit tc_optional_base ( in_place_init_if_t, bool cond, Args&&... args )
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
       boost_optional_detail::construct<value_type>(factory, boost::addressof(m_storage));
       m_initialized = true ;
     }

    // Constructs in-place using the given typed factory
    template<class Expr>
    void construct ( Expr&& factory, typed_in_place_factory_base const* )
     {
       factory.apply(boost::addressof(m_storage)) ;
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
      m_storage = value_type(optional_detail::forward<Expr>(expr)) ;
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

    // Notice that 'Expr' will be optional<T> or optional<U> (but not tc_optional_base<..>)
    template<class Expr>
    void construct ( Expr&& expr, optional_tag const* )
     {
       if ( expr.is_initialized() )
       {
         // An exception can be thrown here.
         // It it happens, THIS will be left uninitialized.
         m_storage = value_type(optional_detail::move(expr.get())) ;
         m_initialized = true ;
       }
     }
#endif // defined BOOST_OPTIONAL_WEAK_OVERLOAD_RESOLUTION

    void assign_value ( argument_type val ) { m_storage = val; }
    void assign_value ( rval_reference_type val ) { m_storage = static_cast<rval_reference_type>(val); }

    void destroy()
    {
      m_initialized = false;
    }

    reference_const_type get_impl() const { return m_storage ; }
    reference_type       get_impl()       { return m_storage ; }

    pointer_const_type get_ptr_impl() const { return boost::addressof(m_storage); }
    pointer_type       get_ptr_impl()       { return boost::addressof(m_storage); }

  private :

    bool m_initialized ;
    T    m_storage ;
} ;
