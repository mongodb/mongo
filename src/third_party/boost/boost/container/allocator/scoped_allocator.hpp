//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Pablo Halpern 2009. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2011. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP
#define BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP

#if (defined _MSC_VER) && (_MSC_VER >= 1200)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>
#include <boost/container/allocator/allocator_traits.hpp>
#include <boost/type_traits.hpp>
#include <utility>

namespace boost { namespace container {

template <typename OuterAlloc, typename... InnerAllocs>
class scoped_allocator_adaptor;

template <typename OuterAlloc, typename... InnerAllocs>
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...> make_scoped();

template <typename OuterAlloc, typename... InnerAllocs>
class scoped_allocator_adaptor_base : public OuterAlloc
{
    typedef allocator_traits<OuterAlloc> OuterTraits;

public:
    // Workaround for inability of gcc-4.4.1 to expand InnerAllocs...
//    typedef scoped_allocator_adaptor<InnerAllocs...> inner_allocator_type;
    typedef decltype(make_scoped<InnerAllocs...>()) inner_allocator_type;

    scoped_allocator_adaptor_base();

    template <typename OuterA2>
      scoped_allocator_adaptor_base(OuterA2&& outerAlloc, const InnerAllocs&... innerAllocs);

    template <typename OuterA2>
      scoped_allocator_adaptor_base(const scoped_allocator_adaptor<OuterA2, InnerAllocs...>& other);
    template <typename OuterA2>
      scoped_allocator_adaptor_base(scoped_allocator_adaptor<OuterA2, InnerAllocs...>&& other);
    
    inner_allocator_type&       inner_allocator()
        { return _M_inner_allocs; }
    inner_allocator_type const& inner_allocator() const
        { return _M_inner_allocs; }

    // Allocator propagation functions.
    scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>
    select_on_container_copy_construction() const;

    typedef std::integral_constant<
        bool,
        OuterTraits::propagate_on_container_copy_assignment::value ||
        inner_allocator_type::propagate_on_container_copy_assignment::value
        > propagate_on_container_copy_assignment;
    typedef std::integral_constant<
        bool,
        OuterTraits::propagate_on_container_move_assignment::value ||
        inner_allocator_type::propagate_on_container_move_assignment::value
        > propagate_on_container_move_assignment;
    typedef std::integral_constant<
        bool,
        OuterTraits::propagate_on_container_swap::value ||
        inner_allocator_type::propagate_on_container_swap::value
        > propagate_on_container_swap;

private:
    inner_allocator_type _M_inner_allocs;
};

// Specialization with only one parameter.
template <typename OuterAlloc>
class scoped_allocator_adaptor_base<OuterAlloc> : public OuterAlloc
{
    typedef allocator_traits<OuterAlloc> OuterTraits;
public:
    typedef scoped_allocator_adaptor<OuterAlloc> inner_allocator_type;

    scoped_allocator_adaptor_base();

    template <typename OuterA2>
      scoped_allocator_adaptor_base(OuterA2&& outerAlloc);

    template <typename OuterA2>
      scoped_allocator_adaptor_base(const scoped_allocator_adaptor<OuterA2>& other);
    template <typename OuterA2>
      scoped_allocator_adaptor_base(scoped_allocator_adaptor<OuterA2>&& other);
    
    inner_allocator_type&       inner_allocator()
        { return static_cast<inner_allocator_type&>(*this); }

    inner_allocator_type const& inner_allocator() const
        { return static_cast<const inner_allocator_type&>(*this); }

    // Allocator propagation functions.
    scoped_allocator_adaptor<OuterAlloc>
      select_on_container_copy_construction() const;

    typedef typename OuterTraits::propagate_on_container_copy_assignment propagate_on_container_copy_assignment;
    typedef typename OuterTraits::propagate_on_container_move_assignment propagate_on_container_move_assignment;
    typedef typename OuterTraits::propagate_on_container_swap propagate_on_container_swap;
};

template <typename OuterAlloc, typename... InnerAllocs>
class scoped_allocator_adaptor
    : public scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>
{
    typedef scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...> _Base;
    typedef allocator_traits<OuterAlloc>                              _Traits;

public:
    typedef OuterAlloc                           outer_allocator_type;
    typedef typename _Base::inner_allocator_type inner_allocator_type;
    
    typedef typename allocator_traits<OuterAlloc>::size_type          size_type;
    typedef typename allocator_traits<OuterAlloc>::difference_type    difference_type;
    typedef typename allocator_traits<OuterAlloc>::pointer            pointer;
    typedef typename allocator_traits<OuterAlloc>::const_pointer      const_pointer;
    typedef typename allocator_traits<OuterAlloc>::void_pointer       void_pointer;
    typedef typename allocator_traits<OuterAlloc>::const_void_pointer const_void_pointer;
    typedef typename allocator_traits<OuterAlloc>::value_type          value_type;

    template <typename Tp>
    struct rebind {
        typedef typename allocator_traits<OuterAlloc>::template rebind_traits<Tp> rebound_traits;
        typedef typename rebound_traits::allocator_type rebound_outer; // exposition only
        typedef scoped_allocator_adaptor<rebound_outer, InnerAllocs...> other;
    };

    scoped_allocator_adaptor();
    scoped_allocator_adaptor(const scoped_allocator_adaptor& other);

    template <typename OuterA2>
      scoped_allocator_adaptor(const scoped_allocator_adaptor<OuterA2, InnerAllocs...>& other);
    template <typename OuterA2>
      scoped_allocator_adaptor(scoped_allocator_adaptor<OuterA2, InnerAllocs...>&& other);
    
    template <typename OuterA2>
      scoped_allocator_adaptor(OuterA2&& outerAlloc, const InnerAllocs&... innerAllocs);

    ~scoped_allocator_adaptor();

    inner_allocator_type      & inner_allocator()
        { return _Base::inner_allocator(); }
    inner_allocator_type const& inner_allocator() const
        { return _Base::inner_allocator(); }
    outer_allocator_type      & outer_allocator()
        { return *this; }
    outer_allocator_type const& outer_allocator() const
        { return *this; }

    pointer allocate(size_type n);
    pointer allocate(size_type n, const_void_pointer hint);
    void deallocate(pointer p, size_type n);
    size_type max_size() const;

    template <typename T, typename... Args>
      void construct(T* p, Args&&... args);

    // Specializations to pass inner_allocator to pair::first and pair::second
    template <class T1, class T2>
      void construct(std::pair<T1,T2>* p);
    template <class T1, class T2, class U, class V>
      void construct(std::pair<T1,T2>* p, U&& x, V&& y);
    template <class T1, class T2, class U, class V>
      void construct(std::pair<T1,T2>* p, const std::pair<U, V>& pr);
    template <class T1, class T2, class U, class V>
      void construct(std::pair<T1,T2>* p, std::pair<U, V>&& pr);

    template <typename T>
      void destroy(T* p);
};

template <typename OuterA1, typename OuterA2, typename... InnerAllocs>
inline
bool operator==(const scoped_allocator_adaptor<OuterA1,InnerAllocs...>& a,
                const scoped_allocator_adaptor<OuterA2,InnerAllocs...>& b);

template <typename OuterA1, typename OuterA2, typename... InnerAllocs>
inline
bool operator!=(const scoped_allocator_adaptor<OuterA1,InnerAllocs...>& a,
                const scoped_allocator_adaptor<OuterA2,InnerAllocs...>& b);

///////////////////////////////////////////////////////////////////////////////
// Implementation of scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>
///////////////////////////////////////////////////////////////////////////////

template <typename OuterAlloc, typename... InnerAllocs>
inline
scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>::
    scoped_allocator_adaptor_base()
{
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor_base(OuterA2&&        outerAlloc,
                                    const InnerAllocs&... innerAllocs)
          : OuterAlloc(std::forward<OuterA2>(outerAlloc))
          , _M_inner_allocs(innerAllocs...)
{
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor_base(
          const scoped_allocator_adaptor<OuterA2, InnerAllocs...>& other)
          : OuterAlloc(other.outer_allocator())
          , _M_inner_allocs(other.inner_allocator())
{
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor_base(
          scoped_allocator_adaptor<OuterA2, InnerAllocs...>&& other)
          : OuterAlloc(std::move(other.outer_allocator()))
          , _M_inner_allocs(std::move(other.inner_allocator()))
{
}

template <typename OuterAlloc, typename... InnerAllocs>
inline
scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>
scoped_allocator_adaptor_base<OuterAlloc,InnerAllocs...>::
  select_on_container_copy_construction() const
{
    return scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>(
        allocator_traits<OuterAlloc>::select_on_container_copy_construction(
            this->outer_allocator()),
        allocator_traits<inner_allocator_type>::select_on_container_copy_construction(
            this->inner_allocator()));
}

///////////////////////////////////////////////////////////////////////////////
// Implementation of scoped_allocator_adaptor_base<OuterAlloc> specialization
///////////////////////////////////////////////////////////////////////////////

template <typename OuterAlloc>
inline
scoped_allocator_adaptor_base<OuterAlloc>::
    scoped_allocator_adaptor_base()
{
}

template <typename OuterAlloc>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc>::
     scoped_allocator_adaptor_base(OuterA2&& outerAlloc)
         : OuterAlloc(std::forward<OuterA2>(outerAlloc))
{
}

template <typename OuterAlloc>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc>::
      scoped_allocator_adaptor_base(
          const scoped_allocator_adaptor<OuterA2>& other)
          : OuterAlloc(other.outer_allocator())
{
}

template <typename OuterAlloc>
  template <typename OuterA2>
    scoped_allocator_adaptor_base<OuterAlloc>::
      scoped_allocator_adaptor_base(
          scoped_allocator_adaptor<OuterA2>&& other)
          : OuterAlloc(std::move(other.outer_allocator()))
{
}

// template <typename OuterAlloc>
// inline
// scoped_allocator_adaptor<OuterAlloc>& 
// scoped_allocator_adaptor_base<OuterAlloc>::inner_allocator()
// {
//     return *this;
// }

// template <typename OuterAlloc>
// inline
// scoped_allocator_adaptor<OuterAlloc> const& 
// scoped_allocator_adaptor_base<OuterAlloc>::inner_allocator() cosnt
// {
//     return *this;
// }

template <typename OuterAlloc>
inline
scoped_allocator_adaptor<OuterAlloc>
scoped_allocator_adaptor_base<OuterAlloc>::
select_on_container_copy_construction() const
{
    return
        allocator_traits<OuterAlloc>::select_on_container_copy_construction(
            this->outer_allocator());
}

///////////////////////////////////////////////////////////////////////////////
// Implementation of scoped_allocator_adaptor details
///////////////////////////////////////////////////////////////////////////////

namespace __details {

    // Overload resolution for __has_ctor resolves to this function
    // when _Tp is constructible with _Args.  Returns true_type().
    
    static void* __void_p; // Declared but not defined

    template <typename _Tp, typename... _Args>
    inline
    auto __has_ctor(int, _Args&&... __args) ->
        decltype((new (__void_p) _Tp(__args...), std::true_type()))
        { return std::true_type(); }

    // Overload resolution for __has_ctor resolves to this function
    // when _Tp is not constructible with _Args. Returns false_type().
    template <typename _Tp, typename... _Args>
    auto __has_ctor(_LowPriorityConversion<int>, _Args&&...) ->
        std::false_type
        { return std::false_type(); }

    template <typename _Alloc>
    struct __is_scoped_allocator_imp {
        template <typename T>
        static char test(int, typename T::outer_allocator_type*);
        template <typename T>
        static int test(_LowPriorityConversion<int>, void*);
        static const bool value = (1 == sizeof(test<_Alloc>(0, 0)));
    };

    template <typename _Alloc>
    struct __is_scoped_allocator
        : std::integral_constant<bool, __is_scoped_allocator_imp<_Alloc>::value>
    {
    };

#if 0
    // Called when outer_allocator_type is not a scoped allocator
    // (recursion stop).
    template <typename _Alloc>
    inline
    auto __outermost_alloc(_LowPriorityConversion<int>, _Alloc& __a) ->
        _Alloc&
    {
        return __a;
    }

    // Called when outer_allocator_type is a scoped allocator to
    // return the outermost allocator type.
    template <typename _Alloc>
    inline auto __outermost_alloc(int, _Alloc& __a) ->
        decltype(__outermost_alloc(0,__a.outer_allocator())) 
    {
        return __a.outer_allocator();
    }
#endif

    template <typename _Ignore, typename _OuterAlloc,
              typename _InnerAlloc, typename _Tp, typename... _Args>
    inline void __dispatch_scoped_construct(std::false_type __uses_alloc,
                                            _Ignore         __use_alloc_prefix,
                                            _OuterAlloc&    __outer_alloc,
                                            _InnerAlloc&    __inner_alloc,
                                            _Tp* __p, _Args&&... __args)
    {
        // _Tp doesn't use allocators.  Construct without an
        // allocator argument.
        allocator_traits<_OuterAlloc>::construct(__outer_alloc, __p,
                                               std::forward<_Args>(__args)...);
    }

    template <typename _OuterAlloc,
              typename _InnerAlloc, typename _Tp, typename... _Args>
    inline void __dispatch_scoped_construct(std::true_type  __uses_alloc,
                                            std::true_type  __use_alloc_prefix,
                                            _OuterAlloc&    __outer_alloc,
                                            _InnerAlloc&    __inner_alloc,
                                            _Tp* __p, _Args&&... __args)
    {
        // _Tp doesn't use allocators.  Construct without an
        // allocator argument.
        allocator_traits<_OuterAlloc>::construct(__outer_alloc, __p,
                                                 allocator_arg, __inner_alloc,
                                               std::forward<_Args>(__args)...);
    }

    template <typename _OuterAlloc,
              typename _InnerAlloc, typename _Tp, typename... _Args>
    inline void __dispatch_scoped_construct(std::true_type  __uses_alloc,
                                            std::false_type __use_alloc_prefix,
                                            _OuterAlloc&    __outer_alloc,
                                            _InnerAlloc&    __inner_alloc,
                                            _Tp* __p, _Args&&... __args)
    {
        // If _Tp uses an allocator compatible with _InnerAlloc,
        // but the specific constructor does not have a variant that
        // takes an allocator argument, then program is malformed.
//         static_assert(has_constructor<_Tp, _Args...>::value,
//                       "Cannot pass inner allocator to this constructor");

        allocator_traits<_OuterAlloc>::construct(
            __outer_alloc, __p, std::forward<_Args>(__args)...,
            __inner_alloc);
    }

    template <typename _OuterAlloc, typename _InnerAlloc,
              typename _Tp, typename... _Args>
    inline void __do_scoped_construct(std::false_type __scoped_outer,
                                      _OuterAlloc&    __outer_alloc,
                                      _InnerAlloc&    __inner_alloc,
                                      _Tp* __p, _Args&&... __args)
    {
        // Dispatch construction to the correct __dispatch_scoped_construct()
        // function based on whether _Tp uses an allocator of type
        // _InnerAlloc and, if so, whether there exists the following
        // constructor:
        //   _Tp(allocator_arg_t, _InnerAlloc, Args...).
        auto __uses_alloc = uses_allocator<_Tp, _InnerAlloc>();
        auto __use_alloc_prefix = __has_ctor<_Tp>(0, allocator_arg,
                                               __inner_alloc,
                                               std::forward<_Args>(__args)...);
        __dispatch_scoped_construct(__uses_alloc, __use_alloc_prefix,
                                    __outer_alloc,
                                    __inner_alloc,
                                    __p, std::forward<_Args>(__args)...);
    }

    template <typename _OuterAlloc, typename _InnerAlloc,
              typename _Tp, typename... _Args>
    void __do_scoped_construct(std::true_type  __scoped_outer,
                               _OuterAlloc&    __outer_alloc,
                               _InnerAlloc&    __inner_alloc,
                               _Tp* __p, _Args&&... __args)
    {
        // Use outermost allocator if __outer_alloc is scoped
        typedef typename _OuterAlloc::outer_allocator_type outerouter;
        __do_scoped_construct(__is_scoped_allocator<outerouter>(),
                              __outer_alloc.outer_allocator(),
                              __inner_alloc,
                              __p, std::forward<_Args>(__args)...);
    }

} // end namespace __details

///////////////////////////////////////////////////////////////////////////////
// Implementation of scoped_allocator_adaptor
///////////////////////////////////////////////////////////////////////////////

template <typename OuterAlloc, typename... InnerAllocs>
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    scoped_allocator_adaptor()
{
}

template <typename OuterAlloc, typename... InnerAllocs>
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    scoped_allocator_adaptor(const scoped_allocator_adaptor& other)
        : _Base(other)
{
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor(const scoped_allocator_adaptor<OuterA2,
                               InnerAllocs...>& other)
        : _Base(other)
{
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor(scoped_allocator_adaptor<OuterA2, InnerAllocs...>&& other)
          : _Base(std::move(other))
{
}
    
template <typename OuterAlloc, typename... InnerAllocs>
  template <typename OuterA2>
    scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
      scoped_allocator_adaptor(OuterA2&& outerAlloc, const InnerAllocs&... innerAllocs)
          : _Base(std::forward<OuterA2>(outerAlloc), innerAllocs...)
{
}

template <typename OuterAlloc, typename... InnerAllocs>
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    ~scoped_allocator_adaptor()
{
}

template <typename OuterAlloc, typename... InnerAllocs>
inline typename allocator_traits<OuterAlloc>::pointer
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    allocate(size_type n)
{
    return allocator_traits<OuterAlloc>::allocate(outer_allocator(), n);
}

template <typename OuterAlloc, typename... InnerAllocs>
inline typename allocator_traits<OuterAlloc>::pointer
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    allocate(size_type n, const_void_pointer hint)
{
    return allocator_traits<OuterAlloc>::allocate(outer_allocator(), n, hint);
}

template <typename OuterAlloc, typename... InnerAllocs>
inline void scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    deallocate(pointer p, size_type n)
{
    allocator_traits<OuterAlloc>::deallocate(outer_allocator(), p, n);
}

template <typename OuterAlloc, typename... InnerAllocs>
inline typename allocator_traits<OuterAlloc>::size_type
scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::max_size() const
{
    return allocator_traits<OuterAlloc>::max_size(outer_allocator());
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename T>
    inline void scoped_allocator_adaptor<OuterAlloc, InnerAllocs...>::
    destroy(T* p)
{
    allocator_traits<OuterAlloc>::destroy(outer_allocator(), p);
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <typename T, typename... Args>
    inline
    void scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>::construct(T* p,
                                                             Args&&... args)
{
    __do_scoped_construct(__details::__is_scoped_allocator<OuterAlloc>(),
                          this->outer_allocator(), this->inner_allocator(),
                          p, std::forward<Args>(args)...);
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <class T1, class T2>
  void scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>::construct(
      std::pair<T1,T2>* p)
{
    construct(addressof(p->first));
    try {
        construct(addressof(p->second));
    }
    catch (...) {
        destroy(addressof(p->first));
        throw;
    }
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <class T1, class T2, class U, class V>
  void scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>::construct(
      std::pair<T1,T2>* p, U&& x, V&& y)
{
    construct(addressof(p->first), std::forward<U>(x));
    try {
        construct(addressof(p->second), std::forward<V>(y));
    }
    catch (...) {
        destroy(addressof(p->first));
        throw;
    }
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <class T1, class T2, class U, class V>
  void scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>::construct(
      std::pair<T1,T2>* p, const std::pair<U, V>& pr)
{
    construct(addressof(p->first), pr.first);
    try {
        construct(addressof(p->second), pr.second);
    }
    catch (...) {
        destroy(addressof(p->first));
        throw;
    }
}

template <typename OuterAlloc, typename... InnerAllocs>
  template <class T1, class T2, class U, class V>
  void scoped_allocator_adaptor<OuterAlloc,InnerAllocs...>::construct(
      std::pair<T1,T2>* p, std::pair<U, V>&& pr)
{
    construct(addressof(p->first), std::move(pr.first));
    try {
        construct(addressof(p->second), std::move(pr.second));
    }
    catch (...) {
        destroy(addressof(p->first));
        throw;
    }
}

template <typename OuterA1, typename OuterA2, typename... InnerAllocs>
inline
bool operator==(const scoped_allocator_adaptor<OuterA1,InnerAllocs...>& a,
                const scoped_allocator_adaptor<OuterA2,InnerAllocs...>& b)
{
    return a.outer_allocator() == b.outer_allocator()
        && a.inner_allocator() == b.inner_allocator();
}

template <typename OuterA1, typename OuterA2>
inline
bool operator==(const scoped_allocator_adaptor<OuterA1>& a,
                const scoped_allocator_adaptor<OuterA2>& b)
{
    return a.outer_allocator() == b.outer_allocator();
}

template <typename OuterA1, typename OuterA2, typename... InnerAllocs>
inline
bool operator!=(const scoped_allocator_adaptor<OuterA1,InnerAllocs...>& a,
                const scoped_allocator_adaptor<OuterA2,InnerAllocs...>& b)
{
    return ! (a == b);
}

}} // namespace boost { namespace container {

#include <boost/container/detail/config_end.hpp>

#endif //  BOOST_CONTAINER_ALLOCATOR_SCOPED_ALLOCATOR_HPP
