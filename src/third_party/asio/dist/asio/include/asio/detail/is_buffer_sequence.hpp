//
// detail/is_buffer_sequence.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_IS_BUFFER_SEQUENCE_HPP
#define ASIO_DETAIL_IS_BUFFER_SEQUENCE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include "asio/detail/type_traits.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {

class mutable_buffer;
class const_buffer;
class mutable_registered_buffer;
class const_registered_buffer;

namespace detail {

struct buffer_sequence_memfns_base
{
  void begin();
  void end();
  void size();
  void max_size();
  void capacity();
  void data();
  void prepare();
  void commit();
  void consume();
  void grow();
  void shrink();
};

template <typename T>
struct buffer_sequence_memfns_derived
  : T, buffer_sequence_memfns_base
{
};

template <typename T, T>
struct buffer_sequence_memfns_check
{
};

template <typename>
char buffer_sequence_begin_helper(...);

template <typename T>
char (&buffer_sequence_begin_helper(T* t,
    enable_if_t<!is_same<
      decltype(asio::buffer_sequence_begin(*t)),
        void>::value>*))[2];

template <typename>
char buffer_sequence_end_helper(...);

template <typename T>
char (&buffer_sequence_end_helper(T* t,
    enable_if_t<!is_same<
      decltype(asio::buffer_sequence_end(*t)),
        void>::value>*))[2];

template <typename>
char (&size_memfn_helper(...))[2];

template <typename T>
char size_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::size>*);

template <typename>
char (&max_size_memfn_helper(...))[2];

template <typename T>
char max_size_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::max_size>*);

template <typename>
char (&capacity_memfn_helper(...))[2];

template <typename T>
char capacity_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::capacity>*);

template <typename>
char (&data_memfn_helper(...))[2];

template <typename T>
char data_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::data>*);

template <typename>
char (&prepare_memfn_helper(...))[2];

template <typename T>
char prepare_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::prepare>*);

template <typename>
char (&commit_memfn_helper(...))[2];

template <typename T>
char commit_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::commit>*);

template <typename>
char (&consume_memfn_helper(...))[2];

template <typename T>
char consume_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::consume>*);

template <typename>
char (&grow_memfn_helper(...))[2];

template <typename T>
char grow_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::grow>*);

template <typename>
char (&shrink_memfn_helper(...))[2];

template <typename T>
char shrink_memfn_helper(
    buffer_sequence_memfns_check<
      void (buffer_sequence_memfns_base::*)(),
      &buffer_sequence_memfns_derived<T>::shrink>*);

template <typename, typename>
char (&buffer_sequence_element_type_helper(...))[2];

template <typename T, typename Buffer>
char buffer_sequence_element_type_helper(T* t,
    enable_if_t<is_convertible<
      decltype(*asio::buffer_sequence_begin(*t)),
        Buffer>::value>*);

template <typename>
char (&const_buffers_type_typedef_helper(...))[2];

template <typename T>
char const_buffers_type_typedef_helper(
    typename T::const_buffers_type*);

template <typename>
char (&mutable_buffers_type_typedef_helper(...))[2];

template <typename T>
char mutable_buffers_type_typedef_helper(
    typename T::mutable_buffers_type*);

template <typename T, typename Buffer>
struct is_buffer_sequence_class
  : integral_constant<bool,
      sizeof(buffer_sequence_begin_helper<T>(0, 0)) != 1 &&
      sizeof(buffer_sequence_end_helper<T>(0, 0)) != 1 &&
      sizeof(buffer_sequence_element_type_helper<T, Buffer>(0, 0)) == 1>
{
};

template <typename T, typename Buffer>
struct is_buffer_sequence
  : conditional<is_class<T>::value,
      is_buffer_sequence_class<T, Buffer>,
      false_type>::type
{
};

template <>
struct is_buffer_sequence<mutable_buffer, mutable_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<mutable_buffer, const_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<const_buffer, const_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<const_buffer, mutable_buffer>
  : false_type
{
};

template <>
struct is_buffer_sequence<mutable_registered_buffer, mutable_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<mutable_registered_buffer, const_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<const_registered_buffer, const_buffer>
  : true_type
{
};

template <>
struct is_buffer_sequence<const_registered_buffer, mutable_buffer>
  : false_type
{
};

template <typename T>
struct is_dynamic_buffer_class_v1
  : integral_constant<bool,
      sizeof(size_memfn_helper<T>(0)) != 1 &&
      sizeof(max_size_memfn_helper<T>(0)) != 1 &&
      sizeof(capacity_memfn_helper<T>(0)) != 1 &&
      sizeof(data_memfn_helper<T>(0)) != 1 &&
      sizeof(consume_memfn_helper<T>(0)) != 1 &&
      sizeof(prepare_memfn_helper<T>(0)) != 1 &&
      sizeof(commit_memfn_helper<T>(0)) != 1 &&
      sizeof(const_buffers_type_typedef_helper<T>(0)) == 1 &&
      sizeof(mutable_buffers_type_typedef_helper<T>(0)) == 1>
{
};

template <typename T>
struct is_dynamic_buffer_v1
  : conditional<is_class<T>::value,
      is_dynamic_buffer_class_v1<T>,
      false_type>::type
{
};

template <typename T>
struct is_dynamic_buffer_class_v2
  : integral_constant<bool,
      sizeof(size_memfn_helper<T>(0)) != 1 &&
      sizeof(max_size_memfn_helper<T>(0)) != 1 &&
      sizeof(capacity_memfn_helper<T>(0)) != 1 &&
      sizeof(data_memfn_helper<T>(0)) != 1 &&
      sizeof(consume_memfn_helper<T>(0)) != 1 &&
      sizeof(grow_memfn_helper<T>(0)) != 1 &&
      sizeof(shrink_memfn_helper<T>(0)) != 1 &&
      sizeof(const_buffers_type_typedef_helper<T>(0)) == 1 &&
      sizeof(mutable_buffers_type_typedef_helper<T>(0)) == 1>
{
};

template <typename T>
struct is_dynamic_buffer_v2
  : conditional<is_class<T>::value,
      is_dynamic_buffer_class_v2<T>,
      false_type>::type
{
};

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_IS_BUFFER_SEQUENCE_HPP
