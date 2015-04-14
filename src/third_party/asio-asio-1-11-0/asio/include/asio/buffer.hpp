//
// buffer.hpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_BUFFER_HPP
#define ASIO_BUFFER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "asio/detail/array_fwd.hpp"
#include "asio/detail/is_buffer_sequence.hpp"
#include "asio/detail/throw_exception.hpp"
#include "asio/detail/type_traits.hpp"

#if defined(ASIO_MSVC)
# if defined(_HAS_ITERATOR_DEBUGGING) && (_HAS_ITERATOR_DEBUGGING != 0)
#  if !defined(ASIO_DISABLE_BUFFER_DEBUGGING)
#   define ASIO_ENABLE_BUFFER_DEBUGGING
#  endif // !defined(ASIO_DISABLE_BUFFER_DEBUGGING)
# endif // defined(_HAS_ITERATOR_DEBUGGING)
#endif // defined(ASIO_MSVC)

#if defined(__GNUC__)
# if defined(_GLIBCXX_DEBUG)
#  if !defined(ASIO_DISABLE_BUFFER_DEBUGGING)
#   define ASIO_ENABLE_BUFFER_DEBUGGING
#  endif // !defined(ASIO_DISABLE_BUFFER_DEBUGGING)
# endif // defined(_GLIBCXX_DEBUG)
#endif // defined(__GNUC__)

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
# include "asio/detail/functional.hpp"
#endif // ASIO_ENABLE_BUFFER_DEBUGGING

#if defined(ASIO_HAS_BOOST_WORKAROUND)
# include <boost/detail/workaround.hpp>
# if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582)) \
    || BOOST_WORKAROUND(__SUNPRO_CC, BOOST_TESTED_AT(0x590))
#  define ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND
# endif // BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
        // || BOOST_WORKAROUND(__SUNPRO_CC, BOOST_TESTED_AT(0x590))
#endif // defined(ASIO_HAS_BOOST_WORKAROUND)

#if defined(ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND)
# include "asio/detail/type_traits.hpp"
#endif // defined(ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND)

#include "asio/detail/push_options.hpp"

namespace asio {

class mutable_buffer;
class const_buffer;

namespace detail {
void* buffer_cast_helper(const mutable_buffer&);
const void* buffer_cast_helper(const const_buffer&);
std::size_t buffer_size_helper(const mutable_buffer&);
std::size_t buffer_size_helper(const const_buffer&);
} // namespace detail

/// Holds a buffer that can be modified.
/**
 * The mutable_buffer class provides a safe representation of a buffer that can
 * be modified. It does not own the underlying data, and so is cheap to copy or
 * assign.
 *
 * @par Accessing Buffer Contents
 *
 * The contents of a buffer may be accessed using the @ref buffer_size
 * and @ref buffer_cast functions:
 *
 * @code asio::mutable_buffer b1 = ...;
 * std::size_t s1 = asio::buffer_size(b1);
 * unsigned char* p1 = asio::buffer_cast<unsigned char*>(b1);
 * @endcode
 *
 * The asio::buffer_cast function permits violations of type safety, so
 * uses of it in application code should be carefully considered.
 */
class mutable_buffer
{
public:
  /// Construct an empty buffer.
  mutable_buffer()
    : data_(0),
      size_(0)
  {
  }

  /// Construct a buffer to represent a given memory range.
  mutable_buffer(void* data, std::size_t size)
    : data_(data),
      size_(size)
  {
  }

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  mutable_buffer(void* data, std::size_t size,
      asio::detail::function<void()> debug_check)
    : data_(data),
      size_(size),
      debug_check_(debug_check)
  {
  }

  const asio::detail::function<void()>& get_debug_check() const
  {
    return debug_check_;
  }
#endif // ASIO_ENABLE_BUFFER_DEBUGGING

private:
  friend void* asio::detail::buffer_cast_helper(
      const mutable_buffer& b);
  friend std::size_t asio::detail::buffer_size_helper(
      const mutable_buffer& b);

  void* data_;
  std::size_t size_;

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  asio::detail::function<void()> debug_check_;
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
};

namespace detail {

inline void* buffer_cast_helper(const mutable_buffer& b)
{
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  if (b.size_ && b.debug_check_)
    b.debug_check_();
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
  return b.data_;
}

inline std::size_t buffer_size_helper(const mutable_buffer& b)
{
  return b.size_;
}

} // namespace detail

/// Adapts a single modifiable buffer so that it meets the requirements of the
/// MutableBufferSequence concept.
class mutable_buffers_1
  : public mutable_buffer
{
public:
  /// The type for each element in the list of buffers.
  typedef mutable_buffer value_type;

  /// A random-access iterator type that may be used to read elements.
  typedef const mutable_buffer* const_iterator;

  /// Construct to represent a given memory range.
  mutable_buffers_1(void* data, std::size_t size)
    : mutable_buffer(data, size)
  {
  }

  /// Construct to represent a single modifiable buffer.
  explicit mutable_buffers_1(const mutable_buffer& b)
    : mutable_buffer(b)
  {
  }

  /// Get a random-access iterator to the first element.
  const_iterator begin() const
  {
    return this;
  }

  /// Get a random-access iterator for one past the last element.
  const_iterator end() const
  {
    return begin() + 1;
  }
};

/// Holds a buffer that cannot be modified.
/**
 * The const_buffer class provides a safe representation of a buffer that cannot
 * be modified. It does not own the underlying data, and so is cheap to copy or
 * assign.
 *
 * @par Accessing Buffer Contents
 *
 * The contents of a buffer may be accessed using the @ref buffer_size
 * and @ref buffer_cast functions:
 *
 * @code asio::const_buffer b1 = ...;
 * std::size_t s1 = asio::buffer_size(b1);
 * const unsigned char* p1 = asio::buffer_cast<const unsigned char*>(b1);
 * @endcode
 *
 * The asio::buffer_cast function permits violations of type safety, so
 * uses of it in application code should be carefully considered.
 */
class const_buffer
{
public:
  /// Construct an empty buffer.
  const_buffer()
    : data_(0),
      size_(0)
  {
  }

  /// Construct a buffer to represent a given memory range.
  const_buffer(const void* data, std::size_t size)
    : data_(data),
      size_(size)
  {
  }

  /// Construct a non-modifiable buffer from a modifiable one.
  const_buffer(const mutable_buffer& b)
    : data_(asio::detail::buffer_cast_helper(b)),
      size_(asio::detail::buffer_size_helper(b))
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
      , debug_check_(b.get_debug_check())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
  {
  }

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  const_buffer(const void* data, std::size_t size,
      asio::detail::function<void()> debug_check)
    : data_(data),
      size_(size),
      debug_check_(debug_check)
  {
  }

  const asio::detail::function<void()>& get_debug_check() const
  {
    return debug_check_;
  }
#endif // ASIO_ENABLE_BUFFER_DEBUGGING

private:
  friend const void* asio::detail::buffer_cast_helper(
      const const_buffer& b);
  friend std::size_t asio::detail::buffer_size_helper(
      const const_buffer& b);

  const void* data_;
  std::size_t size_;

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  asio::detail::function<void()> debug_check_;
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
};

namespace detail {

inline const void* buffer_cast_helper(const const_buffer& b)
{
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
  if (b.size_ && b.debug_check_)
    b.debug_check_();
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
  return b.data_;
}

inline std::size_t buffer_size_helper(const const_buffer& b)
{
  return b.size_;
}

} // namespace detail

/// Adapts a single non-modifiable buffer so that it meets the requirements of
/// the ConstBufferSequence concept.
class const_buffers_1
  : public const_buffer
{
public:
  /// The type for each element in the list of buffers.
  typedef const_buffer value_type;

  /// A random-access iterator type that may be used to read elements.
  typedef const const_buffer* const_iterator;

  /// Construct to represent a given memory range.
  const_buffers_1(const void* data, std::size_t size)
    : const_buffer(data, size)
  {
  }

  /// Construct to represent a single non-modifiable buffer.
  explicit const_buffers_1(const const_buffer& b)
    : const_buffer(b)
  {
  }

  /// Get a random-access iterator to the first element.
  const_iterator begin() const
  {
    return this;
  }

  /// Get a random-access iterator for one past the last element.
  const_iterator end() const
  {
    return begin() + 1;
  }
};

/// Trait to determine whether a type satisfies the MutableBufferSequence
/// requirements.
template <typename T>
struct is_mutable_buffer_sequence
#if defined(GENERATING_DOCUMENTATION)
  : integral_constant<bool, automatically_determined>
#else // defined(GENERATING_DOCUMENTATION)
  : asio::detail::is_buffer_sequence<T, mutable_buffer>
#endif // defined(GENERATING_DOCUMENTATION)
{
};

/// Trait to determine whether a type satisfies the ConstBufferSequence
/// requirements.
template <typename T>
struct is_const_buffer_sequence
#if defined(GENERATING_DOCUMENTATION)
  : integral_constant<bool, automatically_determined>
#else // defined(GENERATING_DOCUMENTATION)
  : asio::detail::is_buffer_sequence<T, const_buffer>
#endif // defined(GENERATING_DOCUMENTATION)
{
};

/// Trait to determine whether a type satisfies the DynamicBufferSequence
/// requirements.
template <typename T>
struct is_dynamic_buffer_sequence
#if defined(GENERATING_DOCUMENTATION)
  : integral_constant<bool, automatically_determined>
#else // defined(GENERATING_DOCUMENTATION)
  : asio::detail::is_dynamic_buffer_sequence<T>
#endif // defined(GENERATING_DOCUMENTATION)
{
};

/// (Deprecated: Use the socket/descriptor wait() and async_wait() member
/// functions.) An implementation of both the ConstBufferSequence and
/// MutableBufferSequence concepts to represent a null buffer sequence.
class null_buffers
{
public:
  /// The type for each element in the list of buffers.
  typedef mutable_buffer value_type;

  /// A random-access iterator type that may be used to read elements.
  typedef const mutable_buffer* const_iterator;

  /// Get a random-access iterator to the first element.
  const_iterator begin() const
  {
    return &buf_;
  }

  /// Get a random-access iterator for one past the last element.
  const_iterator end() const
  {
    return &buf_;
  }

private:
  mutable_buffer buf_;
};

/** @defgroup buffer_size asio::buffer_size
 *
 * @brief The asio::buffer_size function determines the total number of
 * bytes in a buffer or buffer sequence.
 */
/*@{*/

/// Get the number of bytes in a modifiable buffer.
inline std::size_t buffer_size(const mutable_buffer& b)
{
  return detail::buffer_size_helper(b);
}

/// Get the number of bytes in a modifiable buffer.
inline std::size_t buffer_size(const mutable_buffers_1& b)
{
  return detail::buffer_size_helper(b);
}

/// Get the number of bytes in a non-modifiable buffer.
inline std::size_t buffer_size(const const_buffer& b)
{
  return detail::buffer_size_helper(b);
}

/// Get the number of bytes in a non-modifiable buffer.
inline std::size_t buffer_size(const const_buffers_1& b)
{
  return detail::buffer_size_helper(b);
}

/// Get the total number of bytes in a buffer sequence.
/** 
 * The @c BufferSequence template parameter may meet either of the @c
 * ConstBufferSequence or @c MutableBufferSequence type requirements.
 */
template <typename BufferSequence>
inline std::size_t buffer_size(const BufferSequence& b,
    typename enable_if<
      is_const_buffer_sequence<BufferSequence>::value
    >::type* = 0)
{
  std::size_t total_buffer_size = 0;

  typename BufferSequence::const_iterator iter = b.begin();
  typename BufferSequence::const_iterator end = b.end();
  for (; iter != end; ++iter)
    total_buffer_size += detail::buffer_size_helper(*iter);

  return total_buffer_size;
}

/*@}*/

/** @defgroup buffer_cast asio::buffer_cast
 *
 * @brief The asio::buffer_cast function is used to obtain a pointer to
 * the underlying memory region associated with a buffer.
 *
 * @par Examples:
 *
 * To access the memory of a non-modifiable buffer, use:
 * @code asio::const_buffer b1 = ...;
 * const unsigned char* p1 = asio::buffer_cast<const unsigned char*>(b1);
 * @endcode
 *
 * To access the memory of a modifiable buffer, use:
 * @code asio::mutable_buffer b2 = ...;
 * unsigned char* p2 = asio::buffer_cast<unsigned char*>(b2);
 * @endcode
 *
 * The asio::buffer_cast function permits violations of type safety, so
 * uses of it in application code should be carefully considered.
 */
/*@{*/

/// Cast a non-modifiable buffer to a specified pointer to POD type.
template <typename PointerToPodType>
inline PointerToPodType buffer_cast(const mutable_buffer& b)
{
  return static_cast<PointerToPodType>(detail::buffer_cast_helper(b));
}

/// Cast a non-modifiable buffer to a specified pointer to POD type.
template <typename PointerToPodType>
inline PointerToPodType buffer_cast(const const_buffer& b)
{
  return static_cast<PointerToPodType>(detail::buffer_cast_helper(b));
}

/*@}*/

/// Create a new modifiable buffer that is offset from the start of another.
/**
 * @relates mutable_buffer
 */
inline mutable_buffer operator+(const mutable_buffer& b, std::size_t start)
{
  if (start > buffer_size(b))
    return mutable_buffer();
  char* new_data = buffer_cast<char*>(b) + start;
  std::size_t new_size = buffer_size(b) - start;
  return mutable_buffer(new_data, new_size
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
      , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
      );
}

/// Create a new modifiable buffer that is offset from the start of another.
/**
 * @relates mutable_buffer
 */
inline mutable_buffer operator+(std::size_t start, const mutable_buffer& b)
{
  if (start > buffer_size(b))
    return mutable_buffer();
  char* new_data = buffer_cast<char*>(b) + start;
  std::size_t new_size = buffer_size(b) - start;
  return mutable_buffer(new_data, new_size
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
      , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
      );
}

/// Create a new non-modifiable buffer that is offset from the start of another.
/**
 * @relates const_buffer
 */
inline const_buffer operator+(const const_buffer& b, std::size_t start)
{
  if (start > buffer_size(b))
    return const_buffer();
  const char* new_data = buffer_cast<const char*>(b) + start;
  std::size_t new_size = buffer_size(b) - start;
  return const_buffer(new_data, new_size
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
      , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
      );
}

/// Create a new non-modifiable buffer that is offset from the start of another.
/**
 * @relates const_buffer
 */
inline const_buffer operator+(std::size_t start, const const_buffer& b)
{
  if (start > buffer_size(b))
    return const_buffer();
  const char* new_data = buffer_cast<const char*>(b) + start;
  std::size_t new_size = buffer_size(b) - start;
  return const_buffer(new_data, new_size
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
      , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
      );
}

#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
namespace detail {

template <typename Iterator>
class buffer_debug_check
{
public:
  buffer_debug_check(Iterator iter)
    : iter_(iter)
  {
  }

  ~buffer_debug_check()
  {
#if defined(ASIO_MSVC) && (ASIO_MSVC == 1400)
    // MSVC 8's string iterator checking may crash in a std::string::iterator
    // object's destructor when the iterator points to an already-destroyed
    // std::string object, unless the iterator is cleared first.
    iter_ = Iterator();
#endif // defined(ASIO_MSVC) && (ASIO_MSVC == 1400)
  }

  void operator()()
  {
    *iter_;
  }

private:
  Iterator iter_;
};

} // namespace detail
#endif // ASIO_ENABLE_BUFFER_DEBUGGING

/** @defgroup buffer asio::buffer
 *
 * @brief The asio::buffer function is used to create a buffer object to
 * represent raw memory, an array of POD elements, a vector of POD elements,
 * or a std::string.
 *
 * A buffer object represents a contiguous region of memory as a 2-tuple
 * consisting of a pointer and size in bytes. A tuple of the form <tt>{void*,
 * size_t}</tt> specifies a mutable (modifiable) region of memory. Similarly, a
 * tuple of the form <tt>{const void*, size_t}</tt> specifies a const
 * (non-modifiable) region of memory. These two forms correspond to the classes
 * mutable_buffer and const_buffer, respectively. To mirror C++'s conversion
 * rules, a mutable_buffer is implicitly convertible to a const_buffer, and the
 * opposite conversion is not permitted.
 *
 * The simplest use case involves reading or writing a single buffer of a
 * specified size:
 *
 * @code sock.send(asio::buffer(data, size)); @endcode
 *
 * In the above example, the return value of asio::buffer meets the
 * requirements of the ConstBufferSequence concept so that it may be directly
 * passed to the socket's write function. A buffer created for modifiable
 * memory also meets the requirements of the MutableBufferSequence concept.
 *
 * An individual buffer may be created from a builtin array, std::vector,
 * std::array or boost::array of POD elements. This helps prevent buffer
 * overruns by automatically determining the size of the buffer:
 *
 * @code char d1[128];
 * size_t bytes_transferred = sock.receive(asio::buffer(d1));
 *
 * std::vector<char> d2(128);
 * bytes_transferred = sock.receive(asio::buffer(d2));
 *
 * std::array<char, 128> d3;
 * bytes_transferred = sock.receive(asio::buffer(d3));
 *
 * boost::array<char, 128> d4;
 * bytes_transferred = sock.receive(asio::buffer(d4)); @endcode
 *
 * In all three cases above, the buffers created are exactly 128 bytes long.
 * Note that a vector is @e never automatically resized when creating or using
 * a buffer. The buffer size is determined using the vector's <tt>size()</tt>
 * member function, and not its capacity.
 *
 * @par Accessing Buffer Contents
 *
 * The contents of a buffer may be accessed using the @ref buffer_size and
 * @ref buffer_cast functions:
 *
 * @code asio::mutable_buffer b1 = ...;
 * std::size_t s1 = asio::buffer_size(b1);
 * unsigned char* p1 = asio::buffer_cast<unsigned char*>(b1);
 *
 * asio::const_buffer b2 = ...;
 * std::size_t s2 = asio::buffer_size(b2);
 * const void* p2 = asio::buffer_cast<const void*>(b2); @endcode
 *
 * The asio::buffer_cast function permits violations of type safety, so
 * uses of it in application code should be carefully considered.
 *
 * For convenience, the @ref buffer_size function also works on buffer
 * sequences (that is, types meeting the ConstBufferSequence or
 * MutableBufferSequence type requirements). In this case, the function returns
 * the total size of all buffers in the sequence.
 *
 * @par Buffer Copying
 *
 * The @ref buffer_copy function may be used to copy raw bytes between
 * individual buffers and buffer sequences.
 *
 * In particular, when used with the @ref buffer_size, the @ref buffer_copy
 * function can be used to linearise a sequence of buffers. For example:
 *
 * @code vector<const_buffer> buffers = ...;
 *
 * vector<unsigned char> data(asio::buffer_size(buffers));
 * asio::buffer_copy(asio::buffer(data), buffers); @endcode
 *
 * Note that @ref buffer_copy is implemented in terms of @c memcpy, and
 * consequently it cannot be used to copy between overlapping memory regions.
 *
 * @par Buffer Invalidation
 *
 * A buffer object does not have any ownership of the memory it refers to. It
 * is the responsibility of the application to ensure the memory region remains
 * valid until it is no longer required for an I/O operation. When the memory
 * is no longer available, the buffer is said to have been invalidated.
 *
 * For the asio::buffer overloads that accept an argument of type
 * std::vector, the buffer objects returned are invalidated by any vector
 * operation that also invalidates all references, pointers and iterators
 * referring to the elements in the sequence (C++ Std, 23.2.4)
 *
 * For the asio::buffer overloads that accept an argument of type
 * std::basic_string, the buffer objects returned are invalidated according to
 * the rules defined for invalidation of references, pointers and iterators
 * referring to elements of the sequence (C++ Std, 21.3).
 *
 * @par Buffer Arithmetic
 *
 * Buffer objects may be manipulated using simple arithmetic in a safe way
 * which helps prevent buffer overruns. Consider an array initialised as
 * follows:
 *
 * @code boost::array<char, 6> a = { 'a', 'b', 'c', 'd', 'e' }; @endcode
 *
 * A buffer object @c b1 created using:
 *
 * @code b1 = asio::buffer(a); @endcode
 *
 * represents the entire array, <tt>{ 'a', 'b', 'c', 'd', 'e' }</tt>. An
 * optional second argument to the asio::buffer function may be used to
 * limit the size, in bytes, of the buffer:
 *
 * @code b2 = asio::buffer(a, 3); @endcode
 *
 * such that @c b2 represents the data <tt>{ 'a', 'b', 'c' }</tt>. Even if the
 * size argument exceeds the actual size of the array, the size of the buffer
 * object created will be limited to the array size.
 *
 * An offset may be applied to an existing buffer to create a new one:
 *
 * @code b3 = b1 + 2; @endcode
 *
 * where @c b3 will set to represent <tt>{ 'c', 'd', 'e' }</tt>. If the offset
 * exceeds the size of the existing buffer, the newly created buffer will be
 * empty.
 *
 * Both an offset and size may be specified to create a buffer that corresponds
 * to a specific range of bytes within an existing buffer:
 *
 * @code b4 = asio::buffer(b1 + 1, 3); @endcode
 *
 * so that @c b4 will refer to the bytes <tt>{ 'b', 'c', 'd' }</tt>.
 *
 * @par Buffers and Scatter-Gather I/O
 *
 * To read or write using multiple buffers (i.e. scatter-gather I/O), multiple
 * buffer objects may be assigned into a container that supports the
 * MutableBufferSequence (for read) or ConstBufferSequence (for write) concepts:
 *
 * @code
 * char d1[128];
 * std::vector<char> d2(128);
 * boost::array<char, 128> d3;
 *
 * boost::array<mutable_buffer, 3> bufs1 = {
 *   asio::buffer(d1),
 *   asio::buffer(d2),
 *   asio::buffer(d3) };
 * bytes_transferred = sock.receive(bufs1);
 *
 * std::vector<const_buffer> bufs2;
 * bufs2.push_back(asio::buffer(d1));
 * bufs2.push_back(asio::buffer(d2));
 * bufs2.push_back(asio::buffer(d3));
 * bytes_transferred = sock.send(bufs2); @endcode
 */
/*@{*/

/// Create a new modifiable buffer from an existing buffer.
/**
 * @returns <tt>mutable_buffers_1(b)</tt>.
 */
inline mutable_buffers_1 buffer(const mutable_buffer& b)
{
  return mutable_buffers_1(b);
}

/// Create a new modifiable buffer from an existing buffer.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     buffer_cast<void*>(b),
 *     min(buffer_size(b), max_size_in_bytes)); @endcode
 */
inline mutable_buffers_1 buffer(const mutable_buffer& b,
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(buffer_cast<void*>(b),
        buffer_size(b) < max_size_in_bytes
        ? buffer_size(b) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer from an existing buffer.
/**
 * @returns <tt>const_buffers_1(b)</tt>.
 */
inline const_buffers_1 buffer(const const_buffer& b)
{
  return const_buffers_1(b);
}

/// Create a new non-modifiable buffer from an existing buffer.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     buffer_cast<const void*>(b),
 *     min(buffer_size(b), max_size_in_bytes)); @endcode
 */
inline const_buffers_1 buffer(const const_buffer& b,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(buffer_cast<const void*>(b),
        buffer_size(b) < max_size_in_bytes
        ? buffer_size(b) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , b.get_debug_check()
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new modifiable buffer that represents the given memory range.
/**
 * @returns <tt>mutable_buffers_1(data, size_in_bytes)</tt>.
 */
inline mutable_buffers_1 buffer(void* data, std::size_t size_in_bytes)
{
  return mutable_buffers_1(mutable_buffer(data, size_in_bytes));
}

/// Create a new non-modifiable buffer that represents the given memory range.
/**
 * @returns <tt>const_buffers_1(data, size_in_bytes)</tt>.
 */
inline const_buffers_1 buffer(const void* data,
    std::size_t size_in_bytes)
{
  return const_buffers_1(const_buffer(data, size_in_bytes));
}

/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     static_cast<void*>(data),
 *     N * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(PodType (&data)[N])
{
  return mutable_buffers_1(mutable_buffer(data, N * sizeof(PodType)));
}
 
/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     static_cast<void*>(data),
 *     min(N * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(PodType (&data)[N],
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(data,
        N * sizeof(PodType) < max_size_in_bytes
        ? N * sizeof(PodType) : max_size_in_bytes));
}
 
/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     static_cast<const void*>(data),
 *     N * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const PodType (&data)[N])
{
  return const_buffers_1(const_buffer(data, N * sizeof(PodType)));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     static_cast<const void*>(data),
 *     min(N * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const PodType (&data)[N],
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data,
        N * sizeof(PodType) < max_size_in_bytes
        ? N * sizeof(PodType) : max_size_in_bytes));
}

#if defined(ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND)

// Borland C++ and Sun Studio think the overloads:
//
//   unspecified buffer(boost::array<PodType, N>& array ...);
//
// and
//
//   unspecified buffer(boost::array<const PodType, N>& array ...);
//
// are ambiguous. This will be worked around by using a buffer_types traits
// class that contains typedefs for the appropriate buffer and container
// classes, based on whether PodType is const or non-const.

namespace detail {

template <bool IsConst>
struct buffer_types_base;

template <>
struct buffer_types_base<false>
{
  typedef mutable_buffer buffer_type;
  typedef mutable_buffers_1 container_type;
};

template <>
struct buffer_types_base<true>
{
  typedef const_buffer buffer_type;
  typedef const_buffers_1 container_type;
};

template <typename PodType>
struct buffer_types
  : public buffer_types_base<is_const<PodType>::value>
{
};

} // namespace detail

template <typename PodType, std::size_t N>
inline typename detail::buffer_types<PodType>::container_type
buffer(boost::array<PodType, N>& data)
{
  typedef typename asio::detail::buffer_types<PodType>::buffer_type
    buffer_type;
  typedef typename asio::detail::buffer_types<PodType>::container_type
    container_type;
  return container_type(
      buffer_type(data.c_array(), data.size() * sizeof(PodType)));
}

template <typename PodType, std::size_t N>
inline typename detail::buffer_types<PodType>::container_type
buffer(boost::array<PodType, N>& data, std::size_t max_size_in_bytes)
{
  typedef typename asio::detail::buffer_types<PodType>::buffer_type
    buffer_type;
  typedef typename asio::detail::buffer_types<PodType>::container_type
    container_type;
  return container_type(
      buffer_type(data.c_array(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

#else // defined(ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND)

/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(boost::array<PodType, N>& data)
{
  return mutable_buffers_1(
      mutable_buffer(data.c_array(), data.size() * sizeof(PodType)));
}

/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(boost::array<PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(data.c_array(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(boost::array<const PodType, N>& data)
{
  return const_buffers_1(
      const_buffer(data.data(), data.size() * sizeof(PodType)));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(boost::array<const PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.data(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

#endif // defined(ASIO_ENABLE_ARRAY_BUFFER_WORKAROUND)

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const boost::array<PodType, N>& data)
{
  return const_buffers_1(
      const_buffer(data.data(), data.size() * sizeof(PodType)));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const boost::array<PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.data(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

#if defined(ASIO_HAS_STD_ARRAY) || defined(GENERATING_DOCUMENTATION)

/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(std::array<PodType, N>& data)
{
  return mutable_buffers_1(
      mutable_buffer(data.data(), data.size() * sizeof(PodType)));
}

/// Create a new modifiable buffer that represents the given POD array.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline mutable_buffers_1 buffer(std::array<PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(data.data(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(std::array<const PodType, N>& data)
{
  return const_buffers_1(
      const_buffer(data.data(), data.size() * sizeof(PodType)));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(std::array<const PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.data(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     data.size() * sizeof(PodType)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const std::array<PodType, N>& data)
{
  return const_buffers_1(
      const_buffer(data.data(), data.size() * sizeof(PodType)));
}

/// Create a new non-modifiable buffer that represents the given POD array.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 */
template <typename PodType, std::size_t N>
inline const_buffers_1 buffer(const std::array<PodType, N>& data,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.data(),
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes));
}

#endif // defined(ASIO_HAS_STD_ARRAY) || defined(GENERATING_DOCUMENTATION)

/// Create a new modifiable buffer that represents the given POD vector.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.size() ? &data[0] : 0,
 *     data.size() * sizeof(PodType)); @endcode
 *
 * @note The buffer is invalidated by any vector operation that would also
 * invalidate iterators.
 */
template <typename PodType, typename Allocator>
inline mutable_buffers_1 buffer(std::vector<PodType, Allocator>& data)
{
  return mutable_buffers_1(
      mutable_buffer(data.size() ? &data[0] : 0, data.size() * sizeof(PodType)
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::vector<PodType, Allocator>::iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new modifiable buffer that represents the given POD vector.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.size() ? &data[0] : 0,
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 *
 * @note The buffer is invalidated by any vector operation that would also
 * invalidate iterators.
 */
template <typename PodType, typename Allocator>
inline mutable_buffers_1 buffer(std::vector<PodType, Allocator>& data,
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(data.size() ? &data[0] : 0,
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::vector<PodType, Allocator>::iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer that represents the given POD vector.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.size() ? &data[0] : 0,
 *     data.size() * sizeof(PodType)); @endcode
 *
 * @note The buffer is invalidated by any vector operation that would also
 * invalidate iterators.
 */
template <typename PodType, typename Allocator>
inline const_buffers_1 buffer(
    const std::vector<PodType, Allocator>& data)
{
  return const_buffers_1(
      const_buffer(data.size() ? &data[0] : 0, data.size() * sizeof(PodType)
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::vector<PodType, Allocator>::const_iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer that represents the given POD vector.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.size() ? &data[0] : 0,
 *     min(data.size() * sizeof(PodType), max_size_in_bytes)); @endcode
 *
 * @note The buffer is invalidated by any vector operation that would also
 * invalidate iterators.
 */
template <typename PodType, typename Allocator>
inline const_buffers_1 buffer(
    const std::vector<PodType, Allocator>& data, std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.size() ? &data[0] : 0,
        data.size() * sizeof(PodType) < max_size_in_bytes
        ? data.size() * sizeof(PodType) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::vector<PodType, Allocator>::const_iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new modifiable buffer that represents the given string.
/**
 * @returns <tt>mutable_buffers_1(data.size() ? &data[0] : 0,
 * data.size() * sizeof(Elem))</tt>.
 *
 * @note The buffer is invalidated by any non-const operation called on the
 * given string object.
 */
template <typename Elem, typename Traits, typename Allocator>
inline mutable_buffers_1 buffer(
    std::basic_string<Elem, Traits, Allocator>& data)
{
  return mutable_buffers_1(mutable_buffer(data.size() ? &data[0] : 0,
        data.size() * sizeof(Elem)
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::basic_string<Elem, Traits, Allocator>::iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer that represents the given string.
/**
 * @returns A mutable_buffers_1 value equivalent to:
 * @code mutable_buffers_1(
 *     data.size() ? &data[0] : 0,
 *     min(data.size() * sizeof(Elem), max_size_in_bytes)); @endcode
 *
 * @note The buffer is invalidated by any non-const operation called on the
 * given string object.
 */
template <typename Elem, typename Traits, typename Allocator>
inline mutable_buffers_1 buffer(
    std::basic_string<Elem, Traits, Allocator>& data,
    std::size_t max_size_in_bytes)
{
  return mutable_buffers_1(
      mutable_buffer(data.size() ? &data[0] : 0,
        data.size() * sizeof(Elem) < max_size_in_bytes
        ? data.size() * sizeof(Elem) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::basic_string<Elem, Traits, Allocator>::iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer that represents the given string.
/**
 * @returns <tt>const_buffers_1(data.data(), data.size() * sizeof(Elem))</tt>.
 *
 * @note The buffer is invalidated by any non-const operation called on the
 * given string object.
 */
template <typename Elem, typename Traits, typename Allocator>
inline const_buffers_1 buffer(
    const std::basic_string<Elem, Traits, Allocator>& data)
{
  return const_buffers_1(const_buffer(data.data(), data.size() * sizeof(Elem)
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::basic_string<Elem, Traits, Allocator>::const_iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/// Create a new non-modifiable buffer that represents the given string.
/**
 * @returns A const_buffers_1 value equivalent to:
 * @code const_buffers_1(
 *     data.data(),
 *     min(data.size() * sizeof(Elem), max_size_in_bytes)); @endcode
 *
 * @note The buffer is invalidated by any non-const operation called on the
 * given string object.
 */
template <typename Elem, typename Traits, typename Allocator>
inline const_buffers_1 buffer(
    const std::basic_string<Elem, Traits, Allocator>& data,
    std::size_t max_size_in_bytes)
{
  return const_buffers_1(
      const_buffer(data.data(),
        data.size() * sizeof(Elem) < max_size_in_bytes
        ? data.size() * sizeof(Elem) : max_size_in_bytes
#if defined(ASIO_ENABLE_BUFFER_DEBUGGING)
        , detail::buffer_debug_check<
            typename std::basic_string<Elem, Traits, Allocator>::const_iterator
          >(data.begin())
#endif // ASIO_ENABLE_BUFFER_DEBUGGING
        ));
}

/*@}*/

/// Adapt a basic_string to the DynamicBufferSequence requirements.
/**
 * Requires that <tt>sizeof(Elem) == 1</tt>.
 */
template <typename Elem, typename Traits, typename Allocator>
class dynamic_string_buffer
{
public:
  /// The type used to represent the input sequence as a list of buffers.
  typedef const_buffers_1 const_buffers_type;

  /// The type used to represent the output sequence as a list of buffers.
  typedef mutable_buffers_1 mutable_buffers_type;

  /// Construct a dynamic buffer from a string.
  /**
   * @param s The string to be used as backing storage for the dynamic buffer.
   * Any existing data in the string is treated as the dynamic buffer's input
   * sequence. The object stores a reference to the string and the user is
   * responsible for ensuring that the string object remains valid until the
   * dynamic_string_buffer object is destroyed.
   *
   * @param maximum_size Specifies a maximum size for the buffer, in bytes.
   */
  explicit dynamic_string_buffer(std::basic_string<Elem, Traits, Allocator>& s,
      std::size_t maximum_size = (std::numeric_limits<std::size_t>::max)())
    : string_(s),
      size_(string_.size()),
      max_size_(maximum_size)
  {
  }

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move construct a dynamic buffer.
  dynamic_string_buffer(dynamic_string_buffer&& other)
    : string_(other.string_),
      size_(other.size_),
      max_size_(other.max_size_)
  {
  }
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Get the size of the input sequence.
  std::size_t size() const ASIO_NOEXCEPT
  {
    return size_;
  }

  /// Get the maximum size of the dynamic buffer.
  /**
   * @returns The allowed maximum of the sum of the sizes of the input sequence
   * and output sequence.
   */
  std::size_t max_size() const ASIO_NOEXCEPT
  {
    return max_size_;
  }

  /// Get the current capacity of the dynamic buffer.
  /**
   * @returns The current total capacity of the buffer, i.e. for both the input
   * sequence and output sequence.
   */
  std::size_t capacity() const ASIO_NOEXCEPT
  {
    return string_.capacity();
  }

  /// Get a list of buffers that represents the input sequence.
  /**
   * @returns An object of type @c const_buffers_type that satisfies
   * ConstBufferSequence requirements, representing the basic_string memory in
   * input sequence.
   *
   * @note The returned object is invalidated by any @c dynamic_string_buffer
   * or @c basic_string member function that modifies the input sequence or
   * output sequence.
   */
  const_buffers_type data() const ASIO_NOEXCEPT
  {
    return asio::buffer(string_, size_);
  }

  /// Get a list of buffers that represents the output sequence, with the given
  /// size.
  /**
   * Ensures that the output sequence can accommodate @c n bytes, resizing the
   * basic_string object as necessary.
   *
   * @returns An object of type @c mutable_buffers_type that satisfies
   * MutableBufferSequence requirements, representing basic_string memory
   * at the start of the output sequence of size @c n.
   *
   * @throws std::length_error If <tt>size() + n > max_size()</tt>.
   *
   * @note The returned object is invalidated by any @c dynamic_string_buffer
   * or @c basic_string member function that modifies the input sequence or
   * output sequence.
   */
  mutable_buffers_type prepare(std::size_t n)
  {
    if (size () > max_size() || max_size() - size() < n)
    {
      std::length_error ex("dynamic_string_buffer too long");
      asio::detail::throw_exception(ex);
    }

    string_.resize(size_ + n);

    return asio::buffer(asio::buffer(string_) + size_, n);
  }

  /// Move bytes from the output sequence to the input sequence.
  /**
   * @param n The number of bytes to append from the start of the output
   * sequence to the end of the input sequence. The remainder of the output
   * sequence is discarded.
   *
   * Requires a preceding call <tt>prepare(x)</tt> where <tt>x >= n</tt>, and
   * no intervening operations that modify the input or output sequence.
   *
   * @note If @c n is greater than the size of the output sequence, the entire
   * output sequence is moved to the input sequence and no error is issued.
   */
  void commit(std::size_t n)
  {
    size_ += (std::min)(n, string_.size() - size_);
    string_.resize(size_);
  }

  /// Remove characters from the input sequence.
  /**
   * Removes @c n characters from the beginning of the input sequence.
   *
   * @note If @c n is greater than the size of the input sequence, the entire
   * input sequence is consumed and no error is issued.
   */
  void consume(std::size_t n)
  {
    std::size_t consume_length = (std::min)(n, size_);
    string_.erase(consume_length);
    size_ -= consume_length;
  }

private:
  std::basic_string<Elem, Traits, Allocator>& string_;
  std::size_t size_;
  const std::size_t max_size_;
};

/// Adapt a vector to the DynamicBufferSequence requirements.
/**
 * Requires that <tt>sizeof(Elem) == 1</tt>.
 */
template <typename Elem, typename Allocator>
class dynamic_vector_buffer
{
public:
  /// The type used to represent the input sequence as a list of buffers.
  typedef const_buffers_1 const_buffers_type;

  /// The type used to represent the output sequence as a list of buffers.
  typedef mutable_buffers_1 mutable_buffers_type;

  /// Construct a dynamic buffer from a string.
  /**
   * @param v The vector to be used as backing storage for the dynamic buffer.
   * Any existing data in the vector is treated as the dynamic buffer's input
   * sequence. The object stores a reference to the vector and the user is
   * responsible for ensuring that the vector object remains valid until the
   * dynamic_vector_buffer object is destroyed.
   *
   * @param maximum_size Specifies a maximum size for the buffer, in bytes.
   */
  explicit dynamic_vector_buffer(std::vector<Elem, Allocator>& v,
      std::size_t maximum_size = (std::numeric_limits<std::size_t>::max)())
    : vector_(v),
      size_(vector_.size()),
      max_size_(maximum_size)
  {
  }

#if defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move construct a dynamic buffer.
  dynamic_vector_buffer(dynamic_vector_buffer&& other)
    : vector_(other.vector_),
      size_(other.size_),
      max_size_(other.max_size_)
  {
  }
#endif // defined(ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Get the size of the input sequence.
  std::size_t size() const ASIO_NOEXCEPT
  {
    return size_;
  }

  /// Get the maximum size of the dynamic buffer.
  /**
   * @returns The allowed maximum of the sum of the sizes of the input sequence
   * and output sequence.
   */
  std::size_t max_size() const ASIO_NOEXCEPT
  {
    return max_size_;
  }

  /// Get the current capacity of the dynamic buffer.
  /**
   * @returns The current total capacity of the buffer, i.e. for both the input
   * sequence and output sequence.
   */
  std::size_t capacity() const ASIO_NOEXCEPT
  {
    return vector_.capacity();
  }

  /// Get a list of buffers that represents the input sequence.
  /**
   * @returns An object of type @c const_buffers_type that satisfies
   * ConstBufferSequence requirements, representing the basic_string memory in
   * input sequence.
   *
   * @note The returned object is invalidated by any @c dynamic_vector_buffer
   * or @c basic_string member function that modifies the input sequence or
   * output sequence.
   */
  const_buffers_type data() const ASIO_NOEXCEPT
  {
    return asio::buffer(vector_, size_);
  }

  /// Get a list of buffers that represents the output sequence, with the given
  /// size.
  /**
   * Ensures that the output sequence can accommodate @c n bytes, resizing the
   * basic_string object as necessary.
   *
   * @returns An object of type @c mutable_buffers_type that satisfies
   * MutableBufferSequence requirements, representing basic_string memory
   * at the start of the output sequence of size @c n.
   *
   * @throws std::length_error If <tt>size() + n > max_size()</tt>.
   *
   * @note The returned object is invalidated by any @c dynamic_vector_buffer
   * or @c basic_string member function that modifies the input sequence or
   * output sequence.
   */
  mutable_buffers_type prepare(std::size_t n)
  {
    if (size () > max_size() || max_size() - size() < n)
    {
      std::length_error ex("dynamic_vector_buffer too long");
      asio::detail::throw_exception(ex);
    }

    vector_.resize(size_ + n);

    return asio::buffer(asio::buffer(vector_) + size_, n);
  }

  /// Move bytes from the output sequence to the input sequence.
  /**
   * @param n The number of bytes to append from the start of the output
   * sequence to the end of the input sequence. The remainder of the output
   * sequence is discarded.
   *
   * Requires a preceding call <tt>prepare(x)</tt> where <tt>x >= n</tt>, and
   * no intervening operations that modify the input or output sequence.
   *
   * @note If @c n is greater than the size of the output sequence, the entire
   * output sequence is moved to the input sequence and no error is issued.
   */
  void commit(std::size_t n)
  {
    size_ += (std::min)(n, vector_.size() - size_);
    vector_.resize(size_);
  }

  /// Remove characters from the input sequence.
  /**
   * Removes @c n characters from the beginning of the input sequence.
   *
   * @note If @c n is greater than the size of the input sequence, the entire
   * input sequence is consumed and no error is issued.
   */
  void consume(std::size_t n)
  {
    std::size_t consume_length = (std::min)(n, size_);
    vector_.erase(consume_length);
    size_ -= consume_length;
  }

private:
  std::vector<Elem, Allocator>& vector_;
  std::size_t size_;
  const std::size_t max_size_;
};

/** @defgroup dynamic_buffer asio::dynamic_buffer
 *
 * @brief The asio::dynamic_buffer function is used to create a
 * dynamically resized buffer from a @c std::basic_string or @c std::vector.
 */
/*@{*/

/// Create a new dynamic buffer that represents the given string.
/**
 * @returns <tt>dynamic_string_buffer<Elem, Traits, Allocator>(data)</tt>.
 */
template <typename Elem, typename Traits, typename Allocator>
inline dynamic_string_buffer<Elem, Traits, Allocator> dynamic_buffer(
    std::basic_string<Elem, Traits, Allocator>& data)
{
  return dynamic_string_buffer<Elem, Traits, Allocator>(data);
}

/// Create a new dynamic buffer that represents the given string.
/**
 * @returns <tt>dynamic_string_buffer<Elem, Traits, Allocator>(data,
 * max_size)</tt>.
 */
template <typename Elem, typename Traits, typename Allocator>
inline dynamic_string_buffer<Elem, Traits, Allocator> dynamic_buffer(
    std::basic_string<Elem, Traits, Allocator>& data, std::size_t max_size)
{
  return dynamic_string_buffer<Elem, Traits, Allocator>(data, max_size);
}

/// Create a new dynamic buffer that represents the given vector.
/**
 * @returns <tt>dynamic_vector_buffer<Elem, Allocator>(data)</tt>.
 */
template <typename Elem, typename Allocator>
inline dynamic_vector_buffer<Elem, Allocator> dynamic_buffer(
    std::vector<Elem, Allocator>& data)
{
  return dynamic_vector_buffer<Elem, Allocator>(data);
}

/// Create a new dynamic buffer that represents the given vector.
/**
 * @returns <tt>dynamic_vector_buffer<Elem, Allocator>(data, max_size)</tt>.
 */
template <typename Elem, typename Allocator>
inline dynamic_vector_buffer<Elem, Allocator> dynamic_buffer(
    std::vector<Elem, Allocator>& data, std::size_t max_size)
{
  return dynamic_vector_buffer<Elem, Allocator>(data, max_size);
}

/*@}*/

/** @defgroup buffer_copy asio::buffer_copy
 *
 * @brief The asio::buffer_copy function is used to copy bytes from a
 * source buffer (or buffer sequence) to a target buffer (or buffer sequence).
 *
 * The @c buffer_copy function is available in two forms:
 *
 * @li A 2-argument form: @c buffer_copy(target, source)
 *
 * @li A 3-argument form: @c buffer_copy(target, source, max_bytes_to_copy)
 *
 * Both forms return the number of bytes actually copied. The number of bytes
 * copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c If specified, @c max_bytes_to_copy.
 *
 * This prevents buffer overflow, regardless of the buffer sizes used in the
 * copy operation.
 *
 * Note that @ref buffer_copy is implemented in terms of @c memcpy, and
 * consequently it cannot be used to copy between overlapping memory regions.
 */
/*@{*/

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const const_buffer& source)
{
  using namespace std; // For memcpy.
  std::size_t target_size = buffer_size(target);
  std::size_t source_size = buffer_size(source);
  std::size_t n = target_size < source_size ? target_size : source_size;
  memcpy(buffer_cast<void*>(target), buffer_cast<const void*>(source), n);
  return n;
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const const_buffers_1& source)
{
  return buffer_copy(target, static_cast<const const_buffer&>(source));
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const mutable_buffer& source)
{
  return buffer_copy(target, const_buffer(source));
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const mutable_buffers_1& source)
{
  return buffer_copy(target, const_buffer(source));
}

/// Copies bytes from a source buffer sequence to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename ConstBufferSequence>
std::size_t buffer_copy(const mutable_buffer& target,
    const ConstBufferSequence& source,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  std::size_t total_bytes_copied = 0;

  typename ConstBufferSequence::const_iterator source_iter = source.begin();
  typename ConstBufferSequence::const_iterator source_end = source.end();

  for (mutable_buffer target_buffer(target);
      buffer_size(target_buffer) && source_iter != source_end; ++source_iter)
  {
    const_buffer source_buffer(*source_iter);
    std::size_t bytes_copied = buffer_copy(target_buffer, source_buffer);
    total_bytes_copied += bytes_copied;
    target_buffer = target_buffer + bytes_copied;
  }

  return total_bytes_copied;
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const const_buffer& source)
{
  return buffer_copy(static_cast<const mutable_buffer&>(target), source);
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const const_buffers_1& source)
{
  return buffer_copy(static_cast<const mutable_buffer&>(target),
      static_cast<const const_buffer&>(source));
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const mutable_buffer& source)
{
  return buffer_copy(static_cast<const mutable_buffer&>(target),
      const_buffer(source));
}

/// Copies bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const mutable_buffers_1& source)
{
  return buffer_copy(static_cast<const mutable_buffer&>(target),
      const_buffer(source));
}

/// Copies bytes from a source buffer sequence to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename ConstBufferSequence>
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const ConstBufferSequence& source,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(static_cast<const mutable_buffer&>(target), source);
}

/// Copies bytes from a source buffer to a target buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
std::size_t buffer_copy(const MutableBufferSequence& target,
    const const_buffer& source,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  std::size_t total_bytes_copied = 0;

  typename MutableBufferSequence::const_iterator target_iter = target.begin();
  typename MutableBufferSequence::const_iterator target_end = target.end();

  for (const_buffer source_buffer(source);
      buffer_size(source_buffer) && target_iter != target_end; ++target_iter)
  {
    mutable_buffer target_buffer(*target_iter);
    std::size_t bytes_copied = buffer_copy(target_buffer, source_buffer);
    total_bytes_copied += bytes_copied;
    source_buffer = source_buffer + bytes_copied;
  }

  return total_bytes_copied;
}

/// Copies bytes from a source buffer to a target buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const const_buffers_1& source,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, static_cast<const const_buffer&>(source));
}

/// Copies bytes from a source buffer to a target buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const mutable_buffer& source,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, const_buffer(source));
}

/// Copies bytes from a source buffer to a target buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const mutable_buffers_1& source,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, const_buffer(source));
}

/// Copies bytes from a source buffer sequence to a target buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence, typename ConstBufferSequence>
std::size_t buffer_copy(const MutableBufferSequence& target,
    const ConstBufferSequence& source,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value &&
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  std::size_t total_bytes_copied = 0;

  typename MutableBufferSequence::const_iterator target_iter = target.begin();
  typename MutableBufferSequence::const_iterator target_end = target.end();
  std::size_t target_buffer_offset = 0;

  typename ConstBufferSequence::const_iterator source_iter = source.begin();
  typename ConstBufferSequence::const_iterator source_end = source.end();
  std::size_t source_buffer_offset = 0;

  while (target_iter != target_end && source_iter != source_end)
  {
    mutable_buffer target_buffer =
      mutable_buffer(*target_iter) + target_buffer_offset;

    const_buffer source_buffer =
      const_buffer(*source_iter) + source_buffer_offset;

    std::size_t bytes_copied = buffer_copy(target_buffer, source_buffer);
    total_bytes_copied += bytes_copied;

    if (bytes_copied == buffer_size(target_buffer))
    {
      ++target_iter;
      target_buffer_offset = 0;
    }
    else
      target_buffer_offset += bytes_copied;

    if (bytes_copied == buffer_size(source_buffer))
    {
      ++source_iter;
      source_buffer_offset = 0;
    }
    else
      source_buffer_offset += bytes_copied;
  }

  return total_bytes_copied;
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const const_buffer& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const const_buffers_1& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const mutable_buffer& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffer& target,
    const mutable_buffers_1& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer sequence to a target
/// buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename ConstBufferSequence>
inline std::size_t buffer_copy(const mutable_buffer& target,
    const ConstBufferSequence& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const const_buffer& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const const_buffers_1& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const mutable_buffer& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const mutable_buffers_1& source, std::size_t max_bytes_to_copy)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer sequence to a target
/// buffer.
/**
 * @param target A modifiable buffer representing the memory region to which
 * the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename ConstBufferSequence>
inline std::size_t buffer_copy(const mutable_buffers_1& target,
    const ConstBufferSequence& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(buffer(target, max_bytes_to_copy), source);
}

/// Copies a limited number of bytes from a source buffer to a target buffer
/// sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const const_buffer& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, buffer(source, max_bytes_to_copy));
}

/// Copies a limited number of bytes from a source buffer to a target buffer
/// sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer representing the memory region from
 * which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const const_buffers_1& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, buffer(source, max_bytes_to_copy));
}

/// Copies a limited number of bytes from a source buffer to a target buffer
/// sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const mutable_buffer& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, buffer(source, max_bytes_to_copy));
}

/// Copies a limited number of bytes from a source buffer to a target buffer
/// sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A modifiable buffer representing the memory region from which
 * the bytes will be copied. The contents of the source buffer will not be
 * modified.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence>
inline std::size_t buffer_copy(const MutableBufferSequence& target,
    const mutable_buffers_1& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value
    >::type* = 0)
{
  return buffer_copy(target, buffer(source, max_bytes_to_copy));
}

/// Copies a limited number of bytes from a source buffer sequence to a target
/// buffer sequence.
/**
 * @param target A modifiable buffer sequence representing the memory regions to
 * which the bytes will be copied.
 *
 * @param source A non-modifiable buffer sequence representing the memory
 * regions from which the bytes will be copied.
 *
 * @param max_bytes_to_copy The maximum number of bytes to be copied.
 *
 * @returns The number of bytes copied.
 *
 * @note The number of bytes copied is the lesser of:
 *
 * @li @c buffer_size(target)
 *
 * @li @c buffer_size(source)
 *
 * @li @c max_bytes_to_copy
 *
 * This function is implemented in terms of @c memcpy, and consequently it
 * cannot be used to copy between overlapping memory regions.
 */
template <typename MutableBufferSequence, typename ConstBufferSequence>
std::size_t buffer_copy(const MutableBufferSequence& target,
    const ConstBufferSequence& source, std::size_t max_bytes_to_copy,
    typename enable_if<
      is_mutable_buffer_sequence<MutableBufferSequence>::value &&
      is_const_buffer_sequence<ConstBufferSequence>::value
    >::type* = 0)
{
  std::size_t total_bytes_copied = 0;

  typename MutableBufferSequence::const_iterator target_iter = target.begin();
  typename MutableBufferSequence::const_iterator target_end = target.end();
  std::size_t target_buffer_offset = 0;

  typename ConstBufferSequence::const_iterator source_iter = source.begin();
  typename ConstBufferSequence::const_iterator source_end = source.end();
  std::size_t source_buffer_offset = 0;

  while (total_bytes_copied != max_bytes_to_copy
      && target_iter != target_end && source_iter != source_end)
  {
    mutable_buffer target_buffer =
      mutable_buffer(*target_iter) + target_buffer_offset;

    const_buffer source_buffer =
      const_buffer(*source_iter) + source_buffer_offset;

    std::size_t bytes_copied = buffer_copy(target_buffer,
        source_buffer, max_bytes_to_copy - total_bytes_copied);
    total_bytes_copied += bytes_copied;

    if (bytes_copied == buffer_size(target_buffer))
    {
      ++target_iter;
      target_buffer_offset = 0;
    }
    else
      target_buffer_offset += bytes_copied;

    if (bytes_copied == buffer_size(source_buffer))
    {
      ++source_iter;
      source_buffer_offset = 0;
    }
    else
      source_buffer_offset += bytes_copied;
  }

  return total_bytes_copied;
}

/*@}*/

} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_BUFFER_HPP
