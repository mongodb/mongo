/* Copyright 2023 Christian Mazakas.
 * Copyright 2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_NODE_HANDLE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_NODE_HANDLE_HPP

#include <boost/unordered/detail/opt_storage.hpp>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <boost/core/allocator_access.hpp>
#include <type_traits>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

template <class Iterator,class NodeType>
struct insert_return_type
{
  Iterator position;
  bool     inserted;
  NodeType node;
};

template <class NodeType>
struct iteratorless_insert_return_type
{
  bool     inserted;
  NodeType node;
};

template <class TypePolicy,class Allocator>
struct node_handle_base
{
  protected:
    using type_policy=TypePolicy;
    using element_type=typename type_policy::element_type;

  public:
    using allocator_type = Allocator;

  private:
    using node_value_type=typename type_policy::value_type;
    element_type p_;
    BOOST_ATTRIBUTE_NO_UNIQUE_ADDRESS opt_storage<Allocator> a_;

    friend struct node_handle_access;

    template<bool B>
    void move_assign_allocator_if(node_handle_base&& nh)noexcept
    {
      move_assign_allocator_if(
        std::integral_constant<bool,B>{}, std::move(nh));
    }

    void move_assign_allocator_if(
      std::true_type, node_handle_base&& nh)noexcept
    {
      al()=std::move(nh.al());
    }

    void move_assign_allocator_if(
      std::false_type, node_handle_base&&)noexcept
    {
    }

protected:
    node_value_type& data()noexcept
    {
      return *(p_.p);
    }

    node_value_type const& data()const noexcept
    {
      return *(p_.p);
    }

    element_type& element()noexcept
    {
      BOOST_ASSERT(!empty());
      return p_;
    }

    element_type const& element()const noexcept
    {
      BOOST_ASSERT(!empty());
      return p_;
    }

    Allocator& al()noexcept
    {
      BOOST_ASSERT(!empty());
      return a_.t_;
    }

    Allocator const& al()const noexcept
    {
      BOOST_ASSERT(!empty());
      return a_.t_;
    }

    void emplace(element_type&& x,Allocator a)
    {
      BOOST_ASSERT(empty());
      auto* p=x.p;
      p_.p=p;
      new(&a_.t_)Allocator(a);
      x.p=nullptr;
    }

    void reset()
    {
      a_.t_.~Allocator();
      p_.p=nullptr;
    }

  public:
    constexpr node_handle_base()noexcept:p_{nullptr}{}

    node_handle_base(node_handle_base&& nh) noexcept
    {
      p_.p = nullptr;
      if (!nh.empty()){
        emplace(std::move(nh.p_),nh.al());
        nh.reset();
      }
    }

    node_handle_base& operator=(node_handle_base&& nh)noexcept
    {
      if(this!=&nh){
        if(empty()){
          if(nh.empty()){                      /* empty(),  nh.empty() */
            /* nothing to do */
          }else{                               /* empty(), !nh.empty() */
            emplace(std::move(nh.p_),std::move(nh.al()));
            nh.reset();
          }
        }else{
          if(nh.empty()){                      /* !empty(),  nh.empty() */
            type_policy::destroy(al(),&p_);
            reset();
          }else{                               /* !empty(), !nh.empty() */
            bool const pocma=
              boost::allocator_propagate_on_container_move_assignment<
                Allocator>::type::value;

            BOOST_ASSERT(pocma||al()==nh.al());

            type_policy::destroy(al(),&p_);
            move_assign_allocator_if<pocma>(std::move(nh));

            p_=std::move(nh.p_);
            nh.reset();
          }
        }
      }else{
        if(empty()){                           /* empty(),  nh.empty() */
          /* nothing to do */
        }else{                                 /* !empty(), !nh.empty() */
          type_policy::destroy(al(),&p_);
          reset();
        }
      }
      return *this;
    }

    ~node_handle_base()
    {
      if(!empty()){
        type_policy::destroy(al(),&p_);
        reset();
      }
    }

    allocator_type get_allocator()const
    {
#if defined(BOOST_GCC)
      /* GCC lifetime analysis incorrectly warns about uninitialized
       * allocator object under some circumstances.
       */
      if(empty())__builtin_unreachable();
#endif
      return al();
    }

    explicit operator bool()const noexcept{ return !empty();}
    BOOST_ATTRIBUTE_NODISCARD bool empty()const noexcept{return p_.p==nullptr;}

    void swap(node_handle_base& nh) noexcept(
      boost::allocator_is_always_equal<Allocator>::type::value||
      boost::allocator_propagate_on_container_swap<Allocator>::type::value)
    {
      if(this!=&nh){
        if(empty()){
          if(nh.empty()) {
            /* nothing to do here */
          } else {
            emplace(std::move(nh.p_), nh.al());
            nh.reset();
          }
        }else{
          if(nh.empty()){
            nh.emplace(std::move(p_),al());
            reset();
          }else{
            bool const pocs=
              boost::allocator_propagate_on_container_swap<
                Allocator>::type::value;

            BOOST_ASSERT(pocs || al()==nh.al());

            using std::swap;
            p_.swap(nh.p_);
            if(pocs)swap(al(),nh.al());
          }
        }
      }
    }

    friend
    void swap(node_handle_base& lhs,node_handle_base& rhs)
      noexcept(noexcept(lhs.swap(rhs)))
    {
      return lhs.swap(rhs);
    }
};

// Internal usage of node_handle_base protected API

struct node_handle_access
{
  template <class TypePolicy, class Allocator>
  using node_type = node_handle_base<TypePolicy, Allocator>;

#if BOOST_WORKAROUND(BOOST_CLANG_VERSION,<190000)
  // https://github.com/llvm/llvm-project/issues/25708

  template <class TypePolicy, class Allocator>
  struct element_type_impl
  {
    using type = typename node_type<TypePolicy, Allocator>::element_type;
  };
  template <class TypePolicy, class Allocator>
  using element_type = typename element_type_impl<TypePolicy, Allocator>::type;
#else
  template <class TypePolicy, class Allocator>
  using element_type = typename node_type<TypePolicy, Allocator>::element_type;
#endif

  template <class TypePolicy, class Allocator>
  static element_type<TypePolicy, Allocator>&
  element(node_type<TypePolicy, Allocator>& nh)noexcept
  {
    return nh.element();
  }

  template <class TypePolicy, class Allocator>
  static element_type<TypePolicy, Allocator>
  const& element(node_type<TypePolicy, Allocator> const& nh)noexcept
  {
    return nh.element();
  }

  template <class TypePolicy, class Allocator>
  static void emplace(
    node_type<TypePolicy, Allocator>& nh,
    element_type<TypePolicy, Allocator>&& x, Allocator a)
  {
    nh.emplace(std::move(x), a);
  }

  template <class TypePolicy,class Allocator>
  static void reset(node_type<TypePolicy, Allocator>& nh)
  {
    nh.reset();
  }
};

template <class TypePolicy, class Allocator>
class node_handle_emplacer_class
{
  using access = node_handle_access;
  using node_type = access::node_type<TypePolicy, Allocator>;
  using element_type = access::element_type<TypePolicy, Allocator>;

  node_type & nh;

public:
  node_handle_emplacer_class(node_type& nh_): nh(nh_) {}

  void operator()(element_type&& x,Allocator a)
  {
    access::emplace(nh, std::move(x), a);
  }
};

template <class TypePolicy, class Allocator>
node_handle_emplacer_class<TypePolicy, Allocator>
node_handle_emplacer(node_handle_base<TypePolicy, Allocator>& nh)
{
  return {nh};
}

}
}
}
}

#endif // BOOST_UNORDERED_DETAIL_FOA_NODE_HANDLE_HPP
