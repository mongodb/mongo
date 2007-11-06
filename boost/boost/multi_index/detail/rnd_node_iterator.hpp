/* Copyright 2003-2006 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_RND_NODE_ITERATOR_HPP
#define BOOST_MULTI_INDEX_DETAIL_RND_NODE_ITERATOR_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/operators.hpp>

#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_member.hpp>
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* Iterator class for node-based indices with random access iterators. */

template<typename Node,typename Derived=mpl::na>
class rnd_node_iterator:
  public random_access_iterator_helper<
    rnd_node_iterator<Node,Derived>,
    typename Node::value_type,
    std::ptrdiff_t,
    const typename Node::value_type*,
    const typename Node::value_type&>
{
public:
  rnd_node_iterator(){}
  explicit rnd_node_iterator(Node* node_):node(node_){}

  const typename Node::value_type& operator*()const
  {
    return node->value();
  }

  friend bool operator==(
    const rnd_node_iterator& x,const rnd_node_iterator& y)
  {
    return x.node==y.node;
  }

  friend bool operator<(
    const rnd_node_iterator& x,const rnd_node_iterator& y)
  {
    return Node::distance(x.node,y.node)>0;
  }

  rnd_node_iterator& operator++()
  {
    Node::increment(node);
    return *this;
  }

  rnd_node_iterator& operator--()
  {
    Node::decrement(node);
    return *this;
  }

  rnd_node_iterator& operator+=(std::ptrdiff_t n)
  {
    Node::advance(node,n);
    return *this;
  }

  rnd_node_iterator& operator-=(std::ptrdiff_t n)
  {
    Node::advance(node,-n);
    return *this;
  }

  friend std::ptrdiff_t operator-(
    const rnd_node_iterator& x,const rnd_node_iterator& y)
  {
    return Node::distance(y.node,x.node);
  }
  
#if !defined(BOOST_MULTI_INDEX_DISABLE_SERIALIZATION)
  /* Serialization. As for why the following is public,
   * see explanation in safe_mode_iterator notes in safe_mode.hpp.
   */

  BOOST_SERIALIZATION_SPLIT_MEMBER()

  typedef typename Node::base_type node_base_type;

  template<class Archive>
  void save(Archive& ar,const unsigned int)const
  {
    node_base_type* bnode=node;
    ar<<serialization::make_nvp("pointer",bnode);
  }

  template<class Archive>
  void load(Archive& ar,const unsigned int)
  {
    node_base_type* bnode;
    ar>>serialization::make_nvp("pointer",bnode);
    node=static_cast<Node*>(bnode);
  }
#endif

  /* get_node is not to be used by the user */

  typedef Node node_type;

  Node* get_node()const{return node;}

private:
  Node* node;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif
