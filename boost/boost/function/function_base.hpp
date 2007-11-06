// Boost.Function library

//  Copyright Douglas Gregor 2001-2006. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org

#ifndef BOOST_FUNCTION_BASE_HEADER
#define BOOST_FUNCTION_BASE_HEADER

#include <stdexcept>
#include <string>
#include <memory>
#include <new>
#include <typeinfo>
#include <boost/config.hpp>
#include <boost/assert.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/composite_traits.hpp>
#include <boost/ref.hpp>
#include <boost/mpl/if.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/type_traits/alignment_of.hpp>
#ifndef BOOST_NO_SFINAE
#  include "boost/utility/enable_if.hpp"
#else
#  include "boost/mpl/bool.hpp"
#endif
#include <boost/function_equal.hpp>

// Borrowed from Boost.Python library: determines the cases where we
// need to use std::type_info::name to compare instead of operator==.
# if (defined(__GNUC__) && __GNUC__ >= 3) \
 || defined(_AIX) \
 || (   defined(__sgi) && defined(__host_mips))
#  include <cstring>
#  define BOOST_FUNCTION_COMPARE_TYPE_ID(X,Y) \
     (std::strcmp((X).name(),(Y).name()) == 0)
# else
#  define BOOST_FUNCTION_COMPARE_TYPE_ID(X,Y) ((X)==(Y))
#endif

#if defined(BOOST_MSVC) && BOOST_MSVC <= 1300 || defined(__ICL) && __ICL <= 600 || defined(__MWERKS__) && __MWERKS__ < 0x2406 && !defined(BOOST_STRICT_CONFIG)
#  define BOOST_FUNCTION_TARGET_FIX(x) x
#else
#  define BOOST_FUNCTION_TARGET_FIX(x)
#endif // not MSVC

#if defined(__sgi) && defined(_COMPILER_VERSION) && _COMPILER_VERSION <= 730 && !defined(BOOST_STRICT_CONFIG)
// Work around a compiler bug.
// boost::python::objects::function has to be seen by the compiler before the
// boost::function class template.
namespace boost { namespace python { namespace objects {
  class function;
}}}
#endif

#if defined (BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)                    \
 || defined(BOOST_BCB_PARTIAL_SPECIALIZATION_BUG)                         \
 || !(BOOST_STRICT_CONFIG || !defined(__SUNPRO_CC) || __SUNPRO_CC > 0x540)
#  define BOOST_FUNCTION_NO_FUNCTION_TYPE_SYNTAX
#endif

#if !BOOST_WORKAROUND(__BORLANDC__, < 0x600)
#  define BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor,Type)              \
      typename ::boost::enable_if_c<(::boost::type_traits::ice_not<          \
                            (::boost::is_integral<Functor>::value)>::value), \
                           Type>::type
#else
// BCC doesn't recognize this depends on a template argument and complains
// about the use of 'typename'
#  define BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor,Type)     \
      ::boost::enable_if_c<(::boost::type_traits::ice_not<          \
                   (::boost::is_integral<Functor>::value)>::value), \
                       Type>::type
#endif

#if !defined(BOOST_FUNCTION_NO_FUNCTION_TYPE_SYNTAX)
namespace boost {

#if defined(__sgi) && defined(_COMPILER_VERSION) && _COMPILER_VERSION <= 730 && !defined(BOOST_STRICT_CONFIG)
// The library shipping with MIPSpro 7.3.1.3m has a broken allocator<void>
class function_base;

template<typename Signature,
         typename Allocator = std::allocator<function_base> >
class function;
#else
template<typename Signature, typename Allocator = std::allocator<void> >
class function;
#endif

template<typename Signature, typename Allocator>
inline void swap(function<Signature, Allocator>& f1,
                 function<Signature, Allocator>& f2)
{
  f1.swap(f2);
}

} // end namespace boost
#endif // have partial specialization

namespace boost {
  namespace detail {
    namespace function {
      class X;

      /**
       * A buffer used to store small function objects in
       * boost::function. It is a union containing function pointers,
       * object pointers, and a structure that resembles a bound
       * member function pointer.
       */
      union function_buffer
      {
        // For pointers to function objects
        void* obj_ptr;

        // For pointers to std::type_info objects
        // (get_functor_type_tag, check_functor_type_tag).
        const void* const_obj_ptr;

        // For function pointers of all kinds
        mutable void (*func_ptr)();

        // For bound member pointers
        struct bound_memfunc_ptr_t {
          void (X::*memfunc_ptr)(int);
          void* obj_ptr;
        } bound_memfunc_ptr;

        // To relax aliasing constraints
        mutable char data;
      };

      /**
       * The unusable class is a placeholder for unused function arguments
       * It is also completely unusable except that it constructable from
       * anything. This helps compilers without partial specialization to
       * handle Boost.Function objects returning void.
       */
      struct unusable
      {
        unusable() {}
        template<typename T> unusable(const T&) {}
      };

      /* Determine the return type. This supports compilers that do not support
       * void returns or partial specialization by silently changing the return
       * type to "unusable".
       */
      template<typename T> struct function_return_type { typedef T type; };

      template<>
      struct function_return_type<void>
      {
        typedef unusable type;
      };

      // The operation type to perform on the given functor/function pointer
      enum functor_manager_operation_type {
        clone_functor_tag,
        destroy_functor_tag,
        check_functor_type_tag,
        get_functor_type_tag
      };

      // Tags used to decide between different types of functions
      struct function_ptr_tag {};
      struct function_obj_tag {};
      struct member_ptr_tag {};
      struct function_obj_ref_tag {};

      template<typename F>
      class get_function_tag
      {
        typedef typename mpl::if_c<(is_pointer<F>::value),
                                   function_ptr_tag,
                                   function_obj_tag>::type ptr_or_obj_tag;

        typedef typename mpl::if_c<(is_member_pointer<F>::value),
                                   member_ptr_tag,
                                   ptr_or_obj_tag>::type ptr_or_obj_or_mem_tag;

        typedef typename mpl::if_c<(is_reference_wrapper<F>::value),
                                   function_obj_ref_tag,
                                   ptr_or_obj_or_mem_tag>::type or_ref_tag;

      public:
        typedef or_ref_tag type;
      };

      // The trivial manager does nothing but return the same pointer (if we
      // are cloning) or return the null pointer (if we are deleting).
      template<typename F>
      struct reference_manager
      {
        static inline void
        get(const function_buffer& in_buffer, function_buffer& out_buffer, 
            functor_manager_operation_type op)
        {
          switch (op) {
          case clone_functor_tag: 
            out_buffer.obj_ptr = in_buffer.obj_ptr;
            return;

          case destroy_functor_tag:
            out_buffer.obj_ptr = 0;
            return;

          case check_functor_type_tag:
            {
              // DPG TBD: Since we're only storing a pointer, it's
              // possible that the user could ask for a base class or
              // derived class. Is that okay?
              const std::type_info& check_type = 
                *static_cast<const std::type_info*>(out_buffer.const_obj_ptr);
              if (BOOST_FUNCTION_COMPARE_TYPE_ID(check_type, typeid(F)))
                out_buffer.obj_ptr = in_buffer.obj_ptr;
              else
                out_buffer.obj_ptr = 0;
            }
            return;

          case get_functor_type_tag:
            out_buffer.const_obj_ptr = &typeid(F);
            return;
          }
        }
      };

      /**
       * Determine if boost::function can use the small-object
       * optimization with the function object type F.
       */
      template<typename F>
      struct function_allows_small_object_optimization
      {
        BOOST_STATIC_CONSTANT
          (bool, 
           value = ((sizeof(F) <= sizeof(function_buffer) &&
                     (alignment_of<function_buffer>::value 
                      % alignment_of<F>::value == 0))));
      };

      /**
       * The functor_manager class contains a static function "manage" which
       * can clone or destroy the given function/function object pointer.
       */
      template<typename Functor, typename Allocator>
      struct functor_manager
      {
      private:
        typedef Functor functor_type;

        // For function pointers, the manager is trivial
        static inline void
        manager(const function_buffer& in_buffer, function_buffer& out_buffer, 
                functor_manager_operation_type op, function_ptr_tag)
        {
          if (op == clone_functor_tag)
            out_buffer.func_ptr = in_buffer.func_ptr;
          else if (op == destroy_functor_tag)
            out_buffer.func_ptr = 0;
          else /* op == check_functor_type_tag */ {
            const std::type_info& check_type = 
              *static_cast<const std::type_info*>(out_buffer.const_obj_ptr);
            if (BOOST_FUNCTION_COMPARE_TYPE_ID(check_type, typeid(Functor)))
              out_buffer.obj_ptr = &in_buffer.func_ptr;
            else
              out_buffer.obj_ptr = 0;
          }
        }

        // Function objects that fit in the small-object buffer.
        static inline void
        manager(const function_buffer& in_buffer, function_buffer& out_buffer, 
                functor_manager_operation_type op, mpl::true_)
        {
          if (op == clone_functor_tag) {
            const functor_type* in_functor = 
              reinterpret_cast<const functor_type*>(&in_buffer.data);
            new ((void*)&out_buffer.data) functor_type(*in_functor);
          } else if (op == destroy_functor_tag) {
            // Some compilers (Borland, vc6, ...) are unhappy with ~functor_type.
            reinterpret_cast<functor_type*>(&out_buffer.data)->~Functor();
          } else /* op == check_functor_type_tag */ {
            const std::type_info& check_type = 
              *static_cast<const std::type_info*>(out_buffer.const_obj_ptr);
            if (BOOST_FUNCTION_COMPARE_TYPE_ID(check_type, typeid(Functor)))
              out_buffer.obj_ptr = &in_buffer.data;
            else
              out_buffer.obj_ptr = 0;
          }
        }
        
        // Function objects that require heap allocation
        static inline void
        manager(const function_buffer& in_buffer, function_buffer& out_buffer, 
                functor_manager_operation_type op, mpl::false_)
        {
#ifndef BOOST_NO_STD_ALLOCATOR
          typedef typename Allocator::template rebind<functor_type>::other
            allocator_type;
          typedef typename allocator_type::pointer pointer_type;
#else
          typedef functor_type* pointer_type;
#endif // BOOST_NO_STD_ALLOCATOR

#  ifndef BOOST_NO_STD_ALLOCATOR
          allocator_type allocator;
#  endif // BOOST_NO_STD_ALLOCATOR

          if (op == clone_functor_tag) {
            // GCC 2.95.3 gets the CV qualifiers wrong here, so we
            // can't do the static_cast that we should do.
            const functor_type* f =
              (const functor_type*)(in_buffer.obj_ptr);

            // Clone the functor
#  ifndef BOOST_NO_STD_ALLOCATOR
            pointer_type copy = allocator.allocate(1);
            allocator.construct(copy, *f);

            // Get back to the original pointer type
            functor_type* new_f = static_cast<functor_type*>(copy);
#  else
            functor_type* new_f = new functor_type(*f);
#  endif // BOOST_NO_STD_ALLOCATOR
            out_buffer.obj_ptr = new_f;
          } else if (op == destroy_functor_tag) {
            /* Cast from the void pointer to the functor pointer type */
            functor_type* f =
              static_cast<functor_type*>(out_buffer.obj_ptr);

#  ifndef BOOST_NO_STD_ALLOCATOR
            /* Cast from the functor pointer type to the allocator's pointer
               type */
            pointer_type victim = static_cast<pointer_type>(f);

            // Destroy and deallocate the functor
            allocator.destroy(victim);
            allocator.deallocate(victim, 1);
#  else
            delete f;
#  endif // BOOST_NO_STD_ALLOCATOR
            out_buffer.obj_ptr = 0;
          } else /* op == check_functor_type_tag */ {
            const std::type_info& check_type = 
              *static_cast<const std::type_info*>(out_buffer.const_obj_ptr);
            if (BOOST_FUNCTION_COMPARE_TYPE_ID(check_type, typeid(Functor)))
              out_buffer.obj_ptr = in_buffer.obj_ptr;
            else
              out_buffer.obj_ptr = 0;
          }
        }

        // For function objects, we determine whether the function
        // object can use the small-object optimization buffer or
        // whether we need to allocate it on the heap.
        static inline void
        manager(const function_buffer& in_buffer, function_buffer& out_buffer, 
                functor_manager_operation_type op, function_obj_tag)
        {
          manager(in_buffer, out_buffer, op,
                  mpl::bool_<(function_allows_small_object_optimization<functor_type>::value)>());
        }

      public:
        /* Dispatch to an appropriate manager based on whether we have a
           function pointer or a function object pointer. */
        static inline void
        manage(const function_buffer& in_buffer, function_buffer& out_buffer, 
               functor_manager_operation_type op)
        {
          typedef typename get_function_tag<functor_type>::type tag_type;
          switch (op) {
          case get_functor_type_tag:
            out_buffer.const_obj_ptr = &typeid(functor_type);
            return;

          default:
            manager(in_buffer, out_buffer, op, tag_type());
            return;
          }
        }
      };

      // A type that is only used for comparisons against zero
      struct useless_clear_type {};

#ifdef BOOST_NO_SFINAE
      // These routines perform comparisons between a Boost.Function
      // object and an arbitrary function object (when the last
      // parameter is mpl::bool_<false>) or against zero (when the
      // last parameter is mpl::bool_<true>). They are only necessary
      // for compilers that don't support SFINAE.
      template<typename Function, typename Functor>
        bool
        compare_equal(const Function& f, const Functor&, int, mpl::bool_<true>)
        { return f.empty(); }

      template<typename Function, typename Functor>
        bool
        compare_not_equal(const Function& f, const Functor&, int,
                          mpl::bool_<true>)
        { return !f.empty(); }

      template<typename Function, typename Functor>
        bool
        compare_equal(const Function& f, const Functor& g, long,
                      mpl::bool_<false>)
        {
          if (const Functor* fp = f.template target<Functor>())
            return function_equal(*fp, g);
          else return false;
        }

      template<typename Function, typename Functor>
        bool
        compare_equal(const Function& f, const reference_wrapper<Functor>& g,
                      int, mpl::bool_<false>)
        {
          if (const Functor* fp = f.template target<Functor>())
            return fp == g.get_pointer();
          else return false;
        }

      template<typename Function, typename Functor>
        bool
        compare_not_equal(const Function& f, const Functor& g, long,
                          mpl::bool_<false>)
        {
          if (const Functor* fp = f.template target<Functor>())
            return !function_equal(*fp, g);
          else return true;
        }

      template<typename Function, typename Functor>
        bool
        compare_not_equal(const Function& f,
                          const reference_wrapper<Functor>& g, int,
                          mpl::bool_<false>)
        {
          if (const Functor* fp = f.template target<Functor>())
            return fp != g.get_pointer();
          else return true;
        }
#endif // BOOST_NO_SFINAE

      /**
       * Stores the "manager" portion of the vtable for a
       * boost::function object.
       */
      struct vtable_base
      {
        vtable_base() : manager(0) { }
        void (*manager)(const function_buffer& in_buffer, 
                        function_buffer& out_buffer, 
                        functor_manager_operation_type op);
      };
    } // end namespace function
  } // end namespace detail

/**
 * The function_base class contains the basic elements needed for the
 * function1, function2, function3, etc. classes. It is common to all
 * functions (and as such can be used to tell if we have one of the
 * functionN objects).
 */
class function_base
{
public:
  function_base() : vtable(0) { }

  /** Determine if the function is empty (i.e., has no target). */
  bool empty() const { return !vtable; }

  /** Retrieve the type of the stored function object, or typeid(void)
      if this is empty. */
  const std::type_info& target_type() const
  {
    if (!vtable) return typeid(void);

    detail::function::function_buffer type;
    vtable->manager(functor, type, detail::function::get_functor_type_tag);
    return *static_cast<const std::type_info*>(type.const_obj_ptr);
  }

  template<typename Functor>
    Functor* target()
    {
      if (!vtable) return 0;

      detail::function::function_buffer type_result;
      type_result.const_obj_ptr = &typeid(Functor);
      vtable->manager(functor, type_result, 
                      detail::function::check_functor_type_tag);
      return static_cast<Functor*>(type_result.obj_ptr);
    }

  template<typename Functor>
#if defined(BOOST_MSVC) && BOOST_WORKAROUND(BOOST_MSVC, < 1300)
    const Functor* target( Functor * = 0 ) const
#else
    const Functor* target() const
#endif
    {
      if (!vtable) return 0;

      detail::function::function_buffer type_result;
      type_result.const_obj_ptr = &typeid(Functor);
      vtable->manager(functor, type_result, 
                      detail::function::check_functor_type_tag);
      // GCC 2.95.3 gets the CV qualifiers wrong here, so we
      // can't do the static_cast that we should do.
      return (const Functor*)(type_result.obj_ptr);
    }

  template<typename F>
    bool contains(const F& f) const
    {
#if defined(BOOST_MSVC) && BOOST_WORKAROUND(BOOST_MSVC, < 1300)
      if (const F* fp = this->target( (F*)0 ))
#else
      if (const F* fp = this->template target<F>())
#endif
      {
        return function_equal(*fp, f);
      } else {
        return false;
      }
    }

#if defined(__GNUC__) && __GNUC__ == 3 && __GNUC_MINOR__ <= 3
  // GCC 3.3 and newer cannot copy with the global operator==, due to
  // problems with instantiation of function return types before it
  // has been verified that the argument types match up.
  template<typename Functor>
    BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
    operator==(Functor g) const
    {
      if (const Functor* fp = target<Functor>())
        return function_equal(*fp, g);
      else return false;
    }

  template<typename Functor>
    BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
    operator!=(Functor g) const
    {
      if (const Functor* fp = target<Functor>())
        return !function_equal(*fp, g);
      else return true;
    }
#endif

public: // should be protected, but GCC 2.95.3 will fail to allow access
  detail::function::vtable_base* vtable;
  mutable detail::function::function_buffer functor;
};

/**
 * The bad_function_call exception class is thrown when a boost::function
 * object is invoked
 */
class bad_function_call : public std::runtime_error
{
public:
  bad_function_call() : std::runtime_error("call to empty boost::function") {}
};

#ifndef BOOST_NO_SFINAE
inline bool operator==(const function_base& f,
                       detail::function::useless_clear_type*)
{
  return f.empty();
}

inline bool operator!=(const function_base& f,
                       detail::function::useless_clear_type*)
{
  return !f.empty();
}

inline bool operator==(detail::function::useless_clear_type*,
                       const function_base& f)
{
  return f.empty();
}

inline bool operator!=(detail::function::useless_clear_type*,
                       const function_base& f)
{
  return !f.empty();
}
#endif

#ifdef BOOST_NO_SFINAE
// Comparisons between boost::function objects and arbitrary function objects
template<typename Functor>
  inline bool operator==(const function_base& f, Functor g)
  {
    typedef mpl::bool_<(is_integral<Functor>::value)> integral;
    return detail::function::compare_equal(f, g, 0, integral());
  }

template<typename Functor>
  inline bool operator==(Functor g, const function_base& f)
  {
    typedef mpl::bool_<(is_integral<Functor>::value)> integral;
    return detail::function::compare_equal(f, g, 0, integral());
  }

template<typename Functor>
  inline bool operator!=(const function_base& f, Functor g)
  {
    typedef mpl::bool_<(is_integral<Functor>::value)> integral;
    return detail::function::compare_not_equal(f, g, 0, integral());
  }

template<typename Functor>
  inline bool operator!=(Functor g, const function_base& f)
  {
    typedef mpl::bool_<(is_integral<Functor>::value)> integral;
    return detail::function::compare_not_equal(f, g, 0, integral());
  }
#else

#  if !(defined(__GNUC__) && __GNUC__ == 3 && __GNUC_MINOR__ <= 3)
// Comparisons between boost::function objects and arbitrary function
// objects. GCC 3.3 and before has an obnoxious bug that prevents this
// from working.
template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator==(const function_base& f, Functor g)
  {
    if (const Functor* fp = f.template target<Functor>())
      return function_equal(*fp, g);
    else return false;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator==(Functor g, const function_base& f)
  {
    if (const Functor* fp = f.template target<Functor>())
      return function_equal(g, *fp);
    else return false;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator!=(const function_base& f, Functor g)
  {
    if (const Functor* fp = f.template target<Functor>())
      return !function_equal(*fp, g);
    else return true;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator!=(Functor g, const function_base& f)
  {
    if (const Functor* fp = f.template target<Functor>())
      return !function_equal(g, *fp);
    else return true;
  }
#  endif

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator==(const function_base& f, reference_wrapper<Functor> g)
  {
    if (const Functor* fp = f.template target<Functor>())
      return fp == g.get_pointer();
    else return false;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator==(reference_wrapper<Functor> g, const function_base& f)
  {
    if (const Functor* fp = f.template target<Functor>())
      return g.get_pointer() == fp;
    else return false;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator!=(const function_base& f, reference_wrapper<Functor> g)
  {
    if (const Functor* fp = f.template target<Functor>())
      return fp != g.get_pointer();
    else return true;
  }

template<typename Functor>
  BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL(Functor, bool)
  operator!=(reference_wrapper<Functor> g, const function_base& f)
  {
    if (const Functor* fp = f.template target<Functor>())
      return g.get_pointer() != fp;
    else return true;
  }

#endif // Compiler supporting SFINAE

namespace detail {
  namespace function {
    inline bool has_empty_target(const function_base* f)
    {
      return f->empty();
    }

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1310)
    inline bool has_empty_target(const void*)
    {
      return false;
    }
#else
    inline bool has_empty_target(...)
    {
      return false;
    }
#endif
  } // end namespace function
} // end namespace detail
} // end namespace boost

#undef BOOST_FUNCTION_ENABLE_IF_NOT_INTEGRAL
#undef BOOST_FUNCTION_COMPARE_TYPE_ID

#endif // BOOST_FUNCTION_BASE_HEADER
