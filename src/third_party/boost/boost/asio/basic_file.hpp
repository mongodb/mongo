//
// basic_file.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2022 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_BASIC_FILE_HPP
#define BOOST_ASIO_BASIC_FILE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if defined(BOOST_ASIO_HAS_FILE) \
  || defined(GENERATING_DOCUMENTATION)

#include <string>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/detail/cstdint.hpp>
#include <boost/asio/detail/handler_type_requirements.hpp>
#include <boost/asio/detail/io_object_impl.hpp>
#include <boost/asio/detail/non_const_lvalue.hpp>
#include <boost/asio/detail/throw_error.hpp>
#include <boost/asio/detail/type_traits.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/file_base.hpp>
#if defined(BOOST_ASIO_HAS_IOCP)
# include <boost/asio/detail/win_iocp_file_service.hpp>
#elif defined(BOOST_ASIO_HAS_IO_URING)
# include <boost/asio/detail/io_uring_file_service.hpp>
#endif

#if defined(BOOST_ASIO_HAS_MOVE)
# include <utility>
#endif // defined(BOOST_ASIO_HAS_MOVE)

#include <boost/asio/detail/push_options.hpp>

namespace boost {
namespace asio {

#if !defined(BOOST_ASIO_BASIC_FILE_FWD_DECL)
#define BOOST_ASIO_BASIC_FILE_FWD_DECL

// Forward declaration with defaulted arguments.
template <typename Executor = any_io_executor>
class basic_file;

#endif // !defined(BOOST_ASIO_BASIC_FILE_FWD_DECL)

/// Provides file functionality.
/**
 * The basic_file class template provides functionality that is common to both
 * stream-oriented and random-access files.
 *
 * @par Thread Safety
 * @e Distinct @e objects: Safe.@n
 * @e Shared @e objects: Unsafe.
 */
template <typename Executor>
class basic_file
  : public file_base
{
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;

  /// Rebinds the file type to another executor.
  template <typename Executor1>
  struct rebind_executor
  {
    /// The file type when rebound to the specified executor.
    typedef basic_file<Executor1> other;
  };

  /// The native representation of a file.
#if defined(GENERATING_DOCUMENTATION)
  typedef implementation_defined native_handle_type;
#elif defined(BOOST_ASIO_HAS_IOCP)
  typedef detail::win_iocp_file_service::native_handle_type native_handle_type;
#elif defined(BOOST_ASIO_HAS_IO_URING)
  typedef detail::io_uring_file_service::native_handle_type native_handle_type;
#endif

  /// Construct a basic_file without opening it.
  /**
   * This constructor initialises a file without opening it.
   *
   * @param ex The I/O executor that the file will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the file.
   */
  explicit basic_file(const executor_type& ex)
    : impl_(0, ex)
  {
  }

  /// Construct a basic_file without opening it.
  /**
   * This constructor initialises a file without opening it.
   *
   * @param context An execution context which provides the I/O executor that
   * the file will use, by default, to dispatch handlers for any asynchronous
   * operations performed on the file.
   */
  template <typename ExecutionContext>
  explicit basic_file(ExecutionContext& context,
      typename constraint<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      >::type = defaulted_constraint())
    : impl_(0, 0, context)
  {
  }

  /// Construct and open a basic_file.
  /**
   * This constructor initialises a file and opens it.
   *
   * @param ex The I/O executor that the file will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the file.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   */
  explicit basic_file(const executor_type& ex,
      const char* path, file_base::flags open_flags)
    : impl_(0, ex)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(), path, open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Construct a basic_file without opening it.
  /**
   * This constructor initialises a file and opens it.
   *
   * @param context An execution context which provides the I/O executor that
   * the file will use, by default, to dispatch handlers for any asynchronous
   * operations performed on the file.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   */
  template <typename ExecutionContext>
  explicit basic_file(ExecutionContext& context,
      const char* path, file_base::flags open_flags,
      typename constraint<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      >::type = defaulted_constraint())
    : impl_(0, 0, context)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(), path, open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Construct and open a basic_file.
  /**
   * This constructor initialises a file and opens it.
   *
   * @param ex The I/O executor that the file will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the file.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   */
  explicit basic_file(const executor_type& ex,
      const std::string& path, file_base::flags open_flags)
    : impl_(0, ex)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(),
        path.c_str(), open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Construct a basic_file without opening it.
  /**
   * This constructor initialises a file and opens it.
   *
   * @param context An execution context which provides the I/O executor that
   * the file will use, by default, to dispatch handlers for any asynchronous
   * operations performed on the file.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   */
  template <typename ExecutionContext>
  explicit basic_file(ExecutionContext& context,
      const std::string& path, file_base::flags open_flags,
      typename constraint<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      >::type = defaulted_constraint())
    : impl_(0, 0, context)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(),
        path.c_str(), open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Construct a basic_file on an existing native file handle.
  /**
   * This constructor initialises a file object to hold an existing native file.
   *
   * @param ex The I/O executor that the file will use, by default, to
   * dispatch handlers for any asynchronous operations performed on the file.
   *
   * @param native_file A native file handle.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  basic_file(const executor_type& ex, const native_handle_type& native_file)
    : impl_(0, ex)
  {
    boost::system::error_code ec;
    impl_.get_service().assign(
        impl_.get_implementation(), native_file, ec);
    boost::asio::detail::throw_error(ec, "assign");
  }

  /// Construct a basic_file on an existing native file.
  /**
   * This constructor initialises a file object to hold an existing native file.
   *
   * @param context An execution context which provides the I/O executor that
   * the file will use, by default, to dispatch handlers for any asynchronous
   * operations performed on the file.
   *
   * @param native_file A native file.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  template <typename ExecutionContext>
  basic_file(ExecutionContext& context, const native_handle_type& native_file,
      typename constraint<
        is_convertible<ExecutionContext&, execution_context&>::value,
        defaulted_constraint
      >::type = defaulted_constraint())
    : impl_(0, 0, context)
  {
    boost::system::error_code ec;
    impl_.get_service().assign(
        impl_.get_implementation(), native_file, ec);
    boost::asio::detail::throw_error(ec, "assign");
  }

#if defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)
  /// Move-construct a basic_file from another.
  /**
   * This constructor moves a file from one object to another.
   *
   * @param other The other basic_file object from which the move will
   * occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_file(const executor_type&) constructor.
   */
  basic_file(basic_file&& other) BOOST_ASIO_NOEXCEPT
    : impl_(std::move(other.impl_))
  {
  }

  /// Move-assign a basic_file from another.
  /**
   * This assignment operator moves a file from one object to another.
   *
   * @param other The other basic_file object from which the move will
   * occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_file(const executor_type&) constructor.
   */
  basic_file& operator=(basic_file&& other)
  {
    impl_ = std::move(other.impl_);
    return *this;
  }

  // All files have access to each other's implementations.
  template <typename Executor1>
  friend class basic_file;

  /// Move-construct a basic_file from a file of another executor type.
  /**
   * This constructor moves a file from one object to another.
   *
   * @param other The other basic_file object from which the move will
   * occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_file(const executor_type&) constructor.
   */
  template <typename Executor1>
  basic_file(basic_file<Executor1>&& other,
      typename constraint<
        is_convertible<Executor1, Executor>::value,
        defaulted_constraint
      >::type = defaulted_constraint())
    : impl_(std::move(other.impl_))
  {
  }

  /// Move-assign a basic_file from a file of another executor type.
  /**
   * This assignment operator moves a file from one object to another.
   *
   * @param other The other basic_file object from which the move will
   * occur.
   *
   * @note Following the move, the moved-from object is in the same state as if
   * constructed using the @c basic_file(const executor_type&) constructor.
   */
  template <typename Executor1>
  typename constraint<
    is_convertible<Executor1, Executor>::value,
    basic_file&
  >::type operator=(basic_file<Executor1> && other)
  {
    basic_file tmp(std::move(other));
    impl_ = std::move(tmp.impl_);
    return *this;
  }
#endif // defined(BOOST_ASIO_HAS_MOVE) || defined(GENERATING_DOCUMENTATION)

  /// Get the executor associated with the object.
  executor_type get_executor() BOOST_ASIO_NOEXCEPT
  {
    return impl_.get_executor();
  }

  /// Open the file using the specified path.
  /**
   * This function opens the file so that it will use the specified path.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * boost::asio::stream_file file(my_context);
   * file.open("/path/to/my/file", boost::asio::stream_file::read_only);
   * @endcode
   */
  void open(const char* path, file_base::flags open_flags)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(), path, open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Open the file using the specified path.
  /**
   * This function opens the file so that it will use the specified path.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * boost::asio::stream_file file(my_context);
   * boost::system::error_code ec;
   * file.open("/path/to/my/file", boost::asio::stream_file::read_only, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  BOOST_ASIO_SYNC_OP_VOID open(const char* path,
      file_base::flags open_flags, boost::system::error_code& ec)
  {
    impl_.get_service().open(impl_.get_implementation(), path, open_flags, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Open the file using the specified path.
  /**
   * This function opens the file so that it will use the specified path.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @par Example
   * @code
   * boost::asio::stream_file file(my_context);
   * file.open("/path/to/my/file", boost::asio::stream_file::read_only);
   * @endcode
   */
  void open(const std::string& path, file_base::flags open_flags)
  {
    boost::system::error_code ec;
    impl_.get_service().open(impl_.get_implementation(),
        path.c_str(), open_flags, ec);
    boost::asio::detail::throw_error(ec, "open");
  }

  /// Open the file using the specified path.
  /**
   * This function opens the file so that it will use the specified path.
   *
   * @param path The path name identifying the file to be opened.
   *
   * @param open_flags A set of flags that determine how the file should be
   * opened.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @par Example
   * @code
   * boost::asio::stream_file file(my_context);
   * boost::system::error_code ec;
   * file.open("/path/to/my/file", boost::asio::stream_file::read_only, ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  BOOST_ASIO_SYNC_OP_VOID open(const std::string& path,
      file_base::flags open_flags, boost::system::error_code& ec)
  {
    impl_.get_service().open(impl_.get_implementation(),
        path.c_str(), open_flags, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Assign an existing native file to the file.
  /*
   * This function opens the file to hold an existing native file.
   *
   * @param native_file A native file.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void assign(const native_handle_type& native_file)
  {
    boost::system::error_code ec;
    impl_.get_service().assign(
        impl_.get_implementation(), native_file, ec);
    boost::asio::detail::throw_error(ec, "assign");
  }

  /// Assign an existing native file to the file.
  /*
   * This function opens the file to hold an existing native file.
   *
   * @param native_file A native file.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID assign(const native_handle_type& native_file,
      boost::system::error_code& ec)
  {
    impl_.get_service().assign(
        impl_.get_implementation(), native_file, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Determine whether the file is open.
  bool is_open() const
  {
    return impl_.get_service().is_open(impl_.get_implementation());
  }

  /// Close the file.
  /**
   * This function is used to close the file. Any asynchronous read or write
   * operations will be cancelled immediately, and will complete with the
   * boost::asio::error::operation_aborted error.
   *
   * @throws boost::system::system_error Thrown on failure. Note that, even if
   * the function indicates an error, the underlying descriptor is closed.
   */
  void close()
  {
    boost::system::error_code ec;
    impl_.get_service().close(impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "close");
  }

  /// Close the file.
  /**
   * This function is used to close the file. Any asynchronous read or write
   * operations will be cancelled immediately, and will complete with the
   * boost::asio::error::operation_aborted error.
   *
   * @param ec Set to indicate what error occurred, if any. Note that, even if
   * the function indicates an error, the underlying descriptor is closed.
   *
   * @par Example
   * @code
   * boost::asio::stream_file file(my_context);
   * ...
   * boost::system::error_code ec;
   * file.close(ec);
   * if (ec)
   * {
   *   // An error occurred.
   * }
   * @endcode
   */
  BOOST_ASIO_SYNC_OP_VOID close(boost::system::error_code& ec)
  {
    impl_.get_service().close(impl_.get_implementation(), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Release ownership of the underlying native file.
  /**
   * This function causes all outstanding asynchronous read and write
   * operations to finish immediately, and the handlers for cancelled
   * operations will be passed the boost::asio::error::operation_aborted error.
   * Ownership of the native file is then transferred to the caller.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note This function is unsupported on Windows versions prior to Windows
   * 8.1, and will fail with boost::asio::error::operation_not_supported on
   * these platforms.
   */
#if defined(BOOST_ASIO_MSVC) && (BOOST_ASIO_MSVC >= 1400) \
  && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0603)
  __declspec(deprecated("This function always fails with "
        "operation_not_supported when used on Windows versions "
        "prior to Windows 8.1."))
#endif
  native_handle_type release()
  {
    boost::system::error_code ec;
    native_handle_type s = impl_.get_service().release(
        impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "release");
    return s;
  }

  /// Release ownership of the underlying native file.
  /**
   * This function causes all outstanding asynchronous read and write
   * operations to finish immediately, and the handlers for cancelled
   * operations will be passed the boost::asio::error::operation_aborted error.
   * Ownership of the native file is then transferred to the caller.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note This function is unsupported on Windows versions prior to Windows
   * 8.1, and will fail with boost::asio::error::operation_not_supported on
   * these platforms.
   */
#if defined(BOOST_ASIO_MSVC) && (BOOST_ASIO_MSVC >= 1400) \
  && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0603)
  __declspec(deprecated("This function always fails with "
        "operation_not_supported when used on Windows versions "
        "prior to Windows 8.1."))
#endif
  native_handle_type release(boost::system::error_code& ec)
  {
    return impl_.get_service().release(impl_.get_implementation(), ec);
  }

  /// Get the native file representation.
  /**
   * This function may be used to obtain the underlying representation of the
   * file. This is intended to allow access to native file functionality
   * that is not otherwise provided.
   */
  native_handle_type native_handle()
  {
    return impl_.get_service().native_handle(impl_.get_implementation());
  }

  /// Cancel all asynchronous operations associated with the file.
  /**
   * This function causes all outstanding asynchronous read and write
   * operations to finish immediately, and the handlers for cancelled
   * operations will be passed the boost::asio::error::operation_aborted error.
   *
   * @throws boost::system::system_error Thrown on failure.
   *
   * @note Calls to cancel() will always fail with
   * boost::asio::error::operation_not_supported when run on Windows XP, Windows
   * Server 2003, and earlier versions of Windows, unless
   * BOOST_ASIO_ENABLE_CANCELIO is defined. However, the CancelIo function has
   * two issues that should be considered before enabling its use:
   *
   * @li It will only cancel asynchronous operations that were initiated in the
   * current thread.
   *
   * @li It can appear to complete without error, but the request to cancel the
   * unfinished operations may be silently ignored by the operating system.
   * Whether it works or not seems to depend on the drivers that are installed.
   *
   * For portable cancellation, consider using the close() function to
   * simultaneously cancel the outstanding operations and close the file.
   *
   * When running on Windows Vista, Windows Server 2008, and later, the
   * CancelIoEx function is always used. This function does not have the
   * problems described above.
   */
#if defined(BOOST_ASIO_MSVC) && (BOOST_ASIO_MSVC >= 1400) \
  && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600) \
  && !defined(BOOST_ASIO_ENABLE_CANCELIO)
  __declspec(deprecated("By default, this function always fails with "
        "operation_not_supported when used on Windows XP, Windows Server 2003, "
        "or earlier. Consult documentation for details."))
#endif
  void cancel()
  {
    boost::system::error_code ec;
    impl_.get_service().cancel(impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "cancel");
  }

  /// Cancel all asynchronous operations associated with the file.
  /**
   * This function causes all outstanding asynchronous read and write
   * operations to finish immediately, and the handlers for cancelled
   * operations will be passed the boost::asio::error::operation_aborted error.
   *
   * @param ec Set to indicate what error occurred, if any.
   *
   * @note Calls to cancel() will always fail with
   * boost::asio::error::operation_not_supported when run on Windows XP, Windows
   * Server 2003, and earlier versions of Windows, unless
   * BOOST_ASIO_ENABLE_CANCELIO is defined. However, the CancelIo function has
   * two issues that should be considered before enabling its use:
   *
   * @li It will only cancel asynchronous operations that were initiated in the
   * current thread.
   *
   * @li It can appear to complete without error, but the request to cancel the
   * unfinished operations may be silently ignored by the operating system.
   * Whether it works or not seems to depend on the drivers that are installed.
   *
   * For portable cancellation, consider using the close() function to
   * simultaneously cancel the outstanding operations and close the file.
   *
   * When running on Windows Vista, Windows Server 2008, and later, the
   * CancelIoEx function is always used. This function does not have the
   * problems described above.
   */
#if defined(BOOST_ASIO_MSVC) && (BOOST_ASIO_MSVC >= 1400) \
  && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600) \
  && !defined(BOOST_ASIO_ENABLE_CANCELIO)
  __declspec(deprecated("By default, this function always fails with "
        "operation_not_supported when used on Windows XP, Windows Server 2003, "
        "or earlier. Consult documentation for details."))
#endif
  BOOST_ASIO_SYNC_OP_VOID cancel(boost::system::error_code& ec)
  {
    impl_.get_service().cancel(impl_.get_implementation(), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Get the size of the file.
  /**
   * This function determines the size of the file, in bytes.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  uint64_t size() const
  {
    boost::system::error_code ec;
    uint64_t s = impl_.get_service().size(impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "size");
    return s;
  }

  /// Get the size of the file.
  /**
   * This function determines the size of the file, in bytes.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  uint64_t size(boost::system::error_code& ec) const
  {
    return impl_.get_service().size(impl_.get_implementation(), ec);
  }

  /// Alter the size of the file.
  /**
   * This function resizes the file to the specified size, in bytes. If the
   * current file size exceeds @c n then any extra data is discarded. If the
   * current size is less than @c n then the file is extended and filled with
   * zeroes.
   *
   * @param n The new size for the file.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void resize(uint64_t n)
  {
    boost::system::error_code ec;
    impl_.get_service().resize(impl_.get_implementation(), n, ec);
    boost::asio::detail::throw_error(ec, "resize");
  }

  /// Alter the size of the file.
  /**
   * This function resizes the file to the specified size, in bytes. If the
   * current file size exceeds @c n then any extra data is discarded. If the
   * current size is less than @c n then the file is extended and filled with
   * zeroes.
   *
   * @param n The new size for the file.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID resize(uint64_t n, boost::system::error_code& ec)
  {
    impl_.get_service().resize(impl_.get_implementation(), n, ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Synchronise the file to disk.
  /**
   * This function synchronises the file data and metadata to disk. Note that
   * the semantics of this synchronisation vary between operation systems.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void sync_all()
  {
    boost::system::error_code ec;
    impl_.get_service().sync_all(impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "sync_all");
  }

  /// Synchronise the file to disk.
  /**
   * This function synchronises the file data and metadata to disk. Note that
   * the semantics of this synchronisation vary between operation systems.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID sync_all(boost::system::error_code& ec)
  {
    impl_.get_service().sync_all(impl_.get_implementation(), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

  /// Synchronise the file data to disk.
  /**
   * This function synchronises the file data to disk. Note that the semantics
   * of this synchronisation vary between operation systems.
   *
   * @throws boost::system::system_error Thrown on failure.
   */
  void sync_data()
  {
    boost::system::error_code ec;
    impl_.get_service().sync_data(impl_.get_implementation(), ec);
    boost::asio::detail::throw_error(ec, "sync_data");
  }

  /// Synchronise the file data to disk.
  /**
   * This function synchronises the file data to disk. Note that the semantics
   * of this synchronisation vary between operation systems.
   *
   * @param ec Set to indicate what error occurred, if any.
   */
  BOOST_ASIO_SYNC_OP_VOID sync_data(boost::system::error_code& ec)
  {
    impl_.get_service().sync_data(impl_.get_implementation(), ec);
    BOOST_ASIO_SYNC_OP_VOID_RETURN(ec);
  }

protected:
  /// Protected destructor to prevent deletion through this type.
  /**
   * This function destroys the file, cancelling any outstanding asynchronous
   * operations associated with the file as if by calling @c cancel.
   */
  ~basic_file()
  {
  }

#if defined(BOOST_ASIO_HAS_IOCP)
  detail::io_object_impl<detail::win_iocp_file_service, Executor> impl_;
#elif defined(BOOST_ASIO_HAS_IO_URING)
  detail::io_object_impl<detail::io_uring_file_service, Executor> impl_;
#endif

private:
  // Disallow copying and assignment.
  basic_file(const basic_file&) BOOST_ASIO_DELETED;
  basic_file& operator=(const basic_file&) BOOST_ASIO_DELETED;
};

} // namespace asio
} // namespace boost

#include <boost/asio/detail/pop_options.hpp>

#endif // defined(BOOST_ASIO_HAS_FILE)
       //   || defined(GENERATING_DOCUMENTATION)

#endif // BOOST_ASIO_BASIC_FILE_HPP
