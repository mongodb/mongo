/*
 *
 * Copyright (c) 1998-2002
 * John Maddock
 *
 * Use, modification and distribution are subject to the 
 * Boost Software License, Version 1.0. (See accompanying file 
 * LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 *
 */

 /*
  *   LOCATION:    see http://www.boost.org for most recent version.
  *   FILE         regex_stack.hpp
  *   VERSION      see <boost/version.hpp>
  *   DESCRIPTION: Implements customised internal regex stacks.
  *                Note this is an internal header file included
  *                by regex.hpp, do not include on its own.
  */

#ifndef BOOST_REGEX_STACK_HPP
#define BOOST_REGEX_STACK_HPP

#ifndef BOOST_REGEX_CONFIG_HPP
#include <boost/regex/config.hpp>
#endif
#ifndef BOOST_REGEX_RAW_BUFFER_HPP
#include <boost/regex/v4/regex_raw_buffer.hpp>
#endif

namespace boost{
   namespace re_detail{

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

//
// class jstack
// simplified stack optimised for push/peek/pop
// operations, we could use std::stack<std::vector<T>> instead...
//
template <class T, class Allocator = BOOST_DEFAULT_ALLOCATOR(T) >
class jstack
{
public:
   typedef typename boost::detail::rebind_allocator<unsigned char, Allocator>::type allocator_type;
private:
   typedef typename boost::detail::rebind_allocator<T, Allocator>::type             T_alloc_type;
   typedef typename T_alloc_type::size_type                              size_type;
   typedef T value_type;
   struct node
   {
      node* next;
      T* start;  // first item
      T* end;    // last item
      T* last;   // end of storage
   };

   //
   // empty base member optimisation:
   struct data : public allocator_type
   {
      padding buf[(sizeof(T) * 16 + sizeof(padding) - 1) / sizeof(padding)];
      data(const Allocator& a) : allocator_type(a){}
   };

   data alloc_inst;
   mutable node* m_stack;
   mutable node* unused;
   node base;
   size_type block_size;

   void BOOST_REGEX_CALL pop_aux()const;
   void BOOST_REGEX_CALL push_aux();

public:
   jstack(size_type n = 64, const Allocator& a = Allocator());

   ~jstack();

   node* BOOST_REGEX_CALL get_node()
   {
      node* new_stack = reinterpret_cast<node*>(alloc_inst.allocate(sizeof(node) + sizeof(T) * block_size));
      BOOST_REGEX_NOEH_ASSERT(new_stack)
      new_stack->last = reinterpret_cast<T*>(new_stack+1);
      new_stack->start = new_stack->end = new_stack->last + block_size;
      new_stack->next = 0;
      return new_stack;
   }

   bool BOOST_REGEX_CALL empty()
   {
      return (m_stack->start == m_stack->end) && (m_stack->next == 0);
   }

   bool BOOST_REGEX_CALL good()
   {
      return (m_stack->start != m_stack->end) || (m_stack->next != 0);
   }

   T& BOOST_REGEX_CALL peek()
   {
      if(m_stack->start == m_stack->end)
         pop_aux();
      return *m_stack->end;
   }

   const T& BOOST_REGEX_CALL peek()const
   {
      if(m_stack->start == m_stack->end)
         pop_aux();
      return *m_stack->end;
   }

   void BOOST_REGEX_CALL pop()
   {
      if(m_stack->start == m_stack->end)
         pop_aux();
      ::boost::re_detail::pointer_destroy(m_stack->end);
      ++(m_stack->end);
   }

   void BOOST_REGEX_CALL pop(T& t)
   {
      if(m_stack->start == m_stack->end)
         pop_aux();
      t = *m_stack->end;
      ::boost::re_detail::pointer_destroy(m_stack->end);
      ++(m_stack->end);
   }

   void BOOST_REGEX_CALL push(const T& t)
   {
      if(m_stack->end == m_stack->last)
         push_aux();
      --(m_stack->end);
      pointer_construct(m_stack->end, t);
   }

};

template <class T, class Allocator>
jstack<T, Allocator>::jstack(size_type n, const Allocator& a)
    : alloc_inst(a)
{
  unused = 0;
  block_size = n;
  m_stack = &base;
  base.last = reinterpret_cast<T*>(alloc_inst.buf);
  base.end = base.start = base.last + 16;
  base.next = 0;
}

template <class T, class Allocator>
void BOOST_REGEX_CALL jstack<T, Allocator>::push_aux()
{
   // make sure we have spare space on TOS:
   register node* new_node;
   if(unused)
   {
      new_node = unused;
      unused = new_node->next;
      new_node->next = m_stack;
      m_stack = new_node;
   }
   else
   {
      new_node = get_node();
      new_node->next = m_stack;
      m_stack = new_node;
   }
}

template <class T, class Allocator>
void BOOST_REGEX_CALL jstack<T, Allocator>::pop_aux()const
{
   // make sure that we have a valid item
   // on TOS:
   BOOST_ASSERT(m_stack->next);
   register node* p = m_stack;
   m_stack = p->next;
   p->next = unused;
   unused = p;
}

template <class T, class Allocator>
jstack<T, Allocator>::~jstack()
{
   node* condemned;
   while(good())
      pop();
   while(unused)
   {
      condemned = unused;
      unused = unused->next;
      alloc_inst.deallocate(reinterpret_cast<unsigned char*>(condemned), sizeof(node) + sizeof(T) * block_size);
   }
   while(m_stack != &base)
   {
      condemned = m_stack;
      m_stack = m_stack->next;
      alloc_inst.deallocate(reinterpret_cast<unsigned char*>(condemned), sizeof(node) + sizeof(T) * block_size);
   }
}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

} // namespace re_detail
} // namespace boost

#endif











