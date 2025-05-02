// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_POPEN_HPP
#define BOOST_PROCESS_V2_POPEN_HPP

#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#if defined(BOOST_PROCESS_V2_STANDALONE)
#include <asio/connect_pipe.hpp>
#include <asio/readable_pipe.hpp>
#include <asio/writable_pipe.hpp>
#else
#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

/// A subprocess with automatically assigned pipes.
/** The purpose os the popen is to provide a convenient way 
 * to use the stdin & stdout of a process. 
 * 
 * @code {.cpp}
 * popen proc(executor, find_executable("addr2line"), {argv[0]});
 * asio::write(proc, asio::buffer("main\n"));
 * std::string line;
 * asio::read_until(proc, asio::dynamic_buffer(line), '\n');
 * @endcode
 * 
 * 
 * Popen can be used as a stream object in other protocols.
 */ 
template<typename Executor = net::any_io_executor>
struct basic_popen : basic_process<Executor>
{
    /// The executor of the process
    using executor_type = Executor;

    /// Rebinds the popen type to another executor.
    template <typename Executor1>
    struct rebind_executor
    {
        /// The pipe type when rebound to the specified executor.
        typedef basic_popen<Executor1> other;
    };

    /// Move construct a popen
    basic_popen(basic_popen &&) = default;
    /// Move assign a popen
    basic_popen& operator=(basic_popen &&) = default;

    /// Move construct a popen and change the executor type.
    template<typename Executor1>
    basic_popen(basic_popen<Executor1>&& lhs)
        : basic_process<Executor>(std::move(lhs)),
                stdin_(std::move(lhs.stdin_)), stdout_(std::move(lhs.stdout_))
    {
    }

    /// Create a closed process handle
    explicit basic_popen(executor_type exec) : basic_process<Executor>{std::move(exec)} {}

    /// Create a closed process handle
    template <typename ExecutionContext>
    explicit basic_popen(ExecutionContext & context,
        typename std::enable_if<
            is_convertible<ExecutionContext&,
                    net::execution_context&>::value, void *>::type = nullptr)
        : basic_process<Executor>{context}
    {
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename ... Inits>
    explicit basic_popen(
            executor_type executor,
            const filesystem::path& exe,
            std::initializer_list<string_view> args,
            Inits&&... inits)
            : basic_process<Executor>(executor)
    {
        this->basic_process<Executor>::operator=(
            default_process_launcher()(
                    this->get_executor(), exe, args,
                    std::forward<Inits>(inits)...,
                    process_stdio{stdin_, stdout_}
                    ));
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename Launcher, typename ... Inits>
    explicit basic_popen(
            Launcher && launcher,
            executor_type executor,
            const filesystem::path& exe,
            std::initializer_list<string_view> args,
            Inits&&... inits)
            : basic_process<Executor>(executor)
    {
        this->basic_process<Executor>::operator=(
            std::forward<Launcher>(launcher)(
                    this->get_executor(), exe, args,
                    std::forward<Inits>(inits)...,
                    process_stdio{stdin_, stdout_}
                    ));
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename Args, typename ... Inits>
    explicit basic_popen(
            executor_type executor,
    const filesystem::path& exe,
            Args&& args, Inits&&... inits)
            : basic_process<Executor>(executor)
    {
        this->basic_process<Executor>::operator=(
                default_process_launcher()(
                        std::move(executor), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_}
                ));
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename Launcher, typename Args, typename ... Inits>
    explicit basic_popen(
            Launcher && launcher, 
            executor_type executor,
            const filesystem::path& exe,
            Args&& args, Inits&&... inits)
            : basic_process<Executor>(executor)
    {
        this->basic_process<Executor>::operator=(
                std::forward<Launcher>(launcher)(
                        std::move(executor), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_}
                ));
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename ExecutionContext, typename ... Inits>
    explicit basic_popen(
            ExecutionContext & context,
            typename std::enable_if<
                std::is_convertible<ExecutionContext&,
                    net::execution_context&>::value,
            const filesystem::path&>::type exe,
            std::initializer_list<string_view> args,
            Inits&&... inits)
            : basic_process<Executor>(context)
    {
        this->basic_process<Executor>::operator=(
                default_process_launcher()(
                        this->get_executor(), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_, {}}
                ));
    }

        /// Construct a child from a property list and launch it using the default process launcher.
    template<typename Launcher, typename ExecutionContext, typename ... Inits>
    explicit basic_popen(
            Launcher && launcher, 
            ExecutionContext & context,
            typename std::enable_if<
                std::is_convertible<ExecutionContext&,
                    net::execution_context&>::value,
            const filesystem::path&>::type exe,
            std::initializer_list<string_view> args,
            Inits&&... inits)
            : basic_process<Executor>(context)
    {
        this->basic_process<Executor>::operator=(
                std::forward<Launcher>(launcher)(
                        this->get_executor(), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_, {}}
                ));
    }

    /// Construct a child from a property list and launch it using the default process launcher.
    template<typename ExecutionContext, typename Args, typename ... Inits>
    explicit basic_popen(
            ExecutionContext & context,
            typename std::enable_if<
                std::is_convertible<ExecutionContext&,
                    net::execution_context&>::value,
            const filesystem::path&>::type exe,
            Args&& args, Inits&&... inits)
            : basic_process<Executor>(context)
    {
        this->basic_process<Executor>::operator=(
                default_process_launcher()(
                        this->get_executor(), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_}
                ));
    }

        /// Construct a child from a property list and launch it using the default process launcher.
    template<typename Launcher, typename ExecutionContext, typename Args, typename ... Inits>
    explicit basic_popen(
            Launcher && launcher, 
            ExecutionContext & context,
            typename std::enable_if<
                std::is_convertible<ExecutionContext&,
                    net::execution_context&>::value,
            const filesystem::path&>::type exe,
            Args&& args, Inits&&... inits)
            : basic_process<Executor>(context)
    {
        this->basic_process<Executor>::operator=(
                std::forward<Launcher>(launcher)(
                        this->get_executor(), exe, args,
                        std::forward<Inits>(inits)...,
                        process_stdio{stdin_, stdout_}
                ));
    }


    /// The type used for stdin on the parent process side.
    using stdin_type = net::basic_writable_pipe<Executor>;
    /// The type used for stdout on the parent process side.
    using stdout_type = net::basic_readable_pipe<Executor>;

    /// Get the stdin pipe.
    stdin_type  & get_stdin()  {return stdin_; }
    /// Get the stdout pipe.
    stdout_type & get_stdout() {return stdout_; }

    /// Get the stdin pipe.
    const stdin_type  & get_stdin()  const {return stdin_; }
    /// Get the stdout pipe.
    const stdout_type & get_stdout() const {return stdout_; }

    /// Write some data to the pipe.
    /**
     * This function is used to write data to the pipe. The function call will
     * block until one or more bytes of the data has been written successfully,
     * or until an error occurs.
     *
     * @param buffers One or more data buffers to be written to the pipe.
     *
     * @returns The number of bytes written.
     *
     * @throws system_error Thrown on failure. An error code of
     * boost::asio::error::eof indicates that the connection was closed by the
     * subprocess.
     *
     * @note The write_some operation may not transmit all of the data to the
     * peer. Consider using the @ref write function if you need to ensure that
     * all data is written before the blocking operation completes.
     *
     * @par Example
     * To write a single data buffer use the @ref buffer function as follows:
     * @code
     * pipe.write_some(boost::asio::buffer(data, size));
     * @endcode
     * See the @ref buffer documentation for information on writing multiple
     * buffers in one go, and how to use it with arrays, boost::array or
     * std::vector.
     */
    template <typename ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers)
    {
        return stdin_.write_some(buffers);
    }

    /// Write some data to the pipe.
    /**
     * This function is used to write data to the pipe. The function call will
     * block until one or more bytes of the data has been written successfully,
     * or until an error occurs.
     *
     * @param buffers One or more data buffers to be written to the pipe.
     *
     * @param ec Set to indicate what error occurred, if any.
     *
     * @returns The number of bytes written. Returns 0 if an error occurred.
     *
     * @note The write_some operation may not transmit all of the data to the
     * subprocess. Consider using the @ref write function if you need to ensure that
     * all data is written before the blocking operation completes.
     */
    template <typename ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers,
                           error_code& ec)
    {
        return stdin_.write_some(buffers, ec);
    }

    /// Start an asynchronous write.
    /**
     * This function is used to asynchronously write data to the pipe. It is an
     * initiating function for an @ref asynchronous_operation, and always returns
     * immediately.
     *
     * @param buffers One or more data buffers to be written to the pipe.
     * Although the buffers object may be copied as necessary, ownership of the
     * underlying memory blocks is retained by the caller, which must guarantee
     * that they remain valid until the completion handler is called.
     *
     * @param token The @ref completion_token that will be used to produce a
     * completion handler, which will be called when the write completes.
     * Potential completion tokens include @ref use_future, @ref use_awaitable,
     * @ref yield_context, or a function object with the correct completion
     * signature. The function signature of the completion handler must be:
     * @code void handler(
     *   const error_code& error, // Result of operation.
     *   std::size_t bytes_transferred // Number of bytes written.
     * ); @endcode
     * Regardless of whether the asynchronous operation completes immediately or
     * not, the completion handler will not be invoked from within this function.
     * On immediate completion, invocation of the handler will be performed in a
     * manner equivalent to using boost::asio::post().
     *
     * @par Completion Signature
     * @code void(error_code, std::size_t) @endcode
     *
     * @note The write operation may not transmit all of the data to the peer.
     * Consider using the @ref async_write function if you need to ensure that all
     * data is written before the asynchronous operation completes.
     *
     * @par Example
     * To write a single data buffer use the @ref buffer function as follows:
     * @code
     * popen.async_write_some(boost::asio::buffer(data, size), handler);
     * @endcode
     * See the @ref buffer documentation for information on writing multiple
     * buffers in one go, and how to use it with arrays, boost::array or
     * std::vector.
     */
    template <typename ConstBufferSequence,
            BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void (error_code, std::size_t))
            WriteToken = net::default_completion_token_t<executor_type>>
    auto async_write_some(const ConstBufferSequence& buffers,
                     WriteToken && token = net::default_completion_token_t<executor_type>())
         -> decltype(std::declval<stdin_type&>().async_write_some(buffers, std::forward<WriteToken>(token)))
    {
        return stdin_.async_write_some(buffers, std::forward<WriteToken>(token));
    }


    /// Read some data from the pipe.
    /**
     * This function is used to read data from the pipe. The function call will
     * block until one or more bytes of data has been read successfully, or until
     * an error occurs.
     *
     * @param buffers One or more buffers into which the data will be read.
     *
     * @returns The number of bytes read.
     *
     * @throws system_error Thrown on failure. An error code of
     * boost::asio::error::eof indicates that the connection was closed by the
     * peer.
     *
     * @note The read_some operation may not read all of the requested number of
     * bytes. Consider using the @ref read function if you need to ensure that
     * the requested amount of data is read before the blocking operation
     * completes.
     *
     * @par Example
     * To read into a single data buffer use the @ref buffer function as follows:
     * @code
     * basic_readable_pipe.read_some(boost::asio::buffer(data, size));
     * @endcode
     * See the @ref buffer documentation for information on reading into multiple
     * buffers in one go, and how to use it with arrays, boost::array or
     * std::vector.
     */
    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers)
    {
        return stdout_.read_some(buffers);
    }

    /// Read some data from the pipe.
    /**
     * This function is used to read data from the pipe. The function call will
     * block until one or more bytes of data has been read successfully, or until
     * an error occurs.
     *
     * @param buffers One or more buffers into which the data will be read.
     *
     * @param ec Set to indicate what error occurred, if any.
     *
     * @returns The number of bytes read. Returns 0 if an error occurred.
     *
     * @note The read_some operation may not read all of the requested number of
     * bytes. Consider using the @ref read function if you need to ensure that
     * the requested amount of data is read before the blocking operation
     * completes.
     */
    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers,
                          error_code& ec)
    {
        return stdout_.read_some(buffers, ec);
    }

    /// Start an asynchronous read.
    /**
     * This function is used to asynchronously read data from the pipe. It is an
     * initiating function for an @ref asynchronous_operation, and always returns
     * immediately.
     *
     * @param buffers One or more buffers into which the data will be read.
     * Although the buffers object may be copied as necessary, ownership of the
     * underlying memory blocks is retained by the caller, which must guarantee
     * that they remain valid until the completion handler is called.
     *
     * @param token The @ref completion_token that will be used to produce a
     * completion handler, which will be called when the read completes.
     * Potential completion tokens include @ref use_future, @ref use_awaitable,
     * @ref yield_context, or a function object with the correct completion
     * signature. The function signature of the completion handler must be:
     * @code void handler(
     *   const error_code& error, // Result of operation.
     *   std::size_t bytes_transferred // Number of bytes read.
     * ); @endcode
     * Regardless of whether the asynchronous operation completes immediately or
     * not, the completion handler will not be invoked from within this function.
     * On immediate completion, invocation of the handler will be performed in a
     * manner equivalent to using boost::asio::post().
     *
     * @par Completion Signature
     * @code void(error_code, std::size_t) @endcode
     *
     * @note The read operation may not read all of the requested number of bytes.
     * Consider using the @ref async_read function if you need to ensure that the
     * requested amount of data is read before the asynchronous operation
     * completes.
     *
     * @par Example
     * To read into a single data buffer use the @ref buffer function as follows:
     * @code
     * basic_readable_pipe.async_read_some(
     *     boost::asio::buffer(data, size), handler);
     * @endcode
     * See the @ref buffer documentation for information on reading into multiple
     * buffers in one go, and how to use it with arrays, boost::array or
     * std::vector.
     */
    template <typename MutableBufferSequence,
            BOOST_PROCESS_V2_COMPLETION_TOKEN_FOR(void (error_code, std::size_t))
            ReadToken = net::default_completion_token_t<executor_type>>
    auto async_read_some(const MutableBufferSequence& buffers,
                    BOOST_ASIO_MOVE_ARG(ReadToken) token
                    = net::default_completion_token_t<executor_type>())
        -> decltype(std::declval<stdout_type&>().async_read_some(buffers, std::forward<ReadToken>(token)))
    {
        return stdout_.async_read_some(buffers, std::forward<ReadToken>(token));
    }



  private:
    stdin_type  stdin_ {basic_process<Executor>::get_executor()};
    stdout_type stdout_{basic_process<Executor>::get_executor()};
};

/// A popen object with the default  executor.
using popen = basic_popen<>;

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_POPEN_HPP
