// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_SHELL_HPP
#define BOOST_PROCESS_V2_SHELL_HPP

#include <boost/core/exchange.hpp>
#include <boost/process/v2/cstring_ref.hpp>
#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/detail/utf8.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/environment.hpp>
#include <memory>
#include <string>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

/// Error category used by the shell parser.
extern BOOST_PROCESS_V2_DECL const error_category& get_shell_category();
static const error_category& shell_category = get_shell_category();

/// Utility to parse commands 
/** This utility class parses command lines into tokens
 * and allows users to executed based on textual inputs.
 * 
 * In v1, this was possible directly when starting a process,
 * but has been removed based on the security risks associated with this.
 * 
 * By making the shell parsing explicitly, it encourages
 * a user to run a sanity check on the executable before launching it.
 * 
 * @par Example
 * @code {.cpp}
 * asio::io_context ctx;
 * 
 * auto cmd = shell("my-app --help");
 * auto exe = cmd.exe();
 * check_if_malicious(exe);
 * 
 * process proc{ctx, exe, cmd.args()};
 * 
 * @endcode
 * 
 * 
 */
struct shell
{    
#if defined(BOOST_PROCESS_V2_WINDOWS)
    using char_type = wchar_t; 
    using args_type = const wchar_t *;
#else
    using char_type = char;
    using args_type = const char **;
#endif

    shell() = default;

    template<typename Char, typename Traits>
    shell(basic_string_view<Char, Traits> input) 
        : buffer_(detail::conv_string<char_type>(input.data(), input.size())) 
    {
        parse_();
    }

    shell(basic_cstring_ref<char_type> input) : input_(input) {parse_();}
    shell(basic_string_view<
                    typename std::conditional<
                        std::is_same<char_type, char>::value,
                        wchar_t, char>::type> input) : buffer_(detail::conv_string<char_type>(input.data(), input.size())) 
    {
        parse_();
    }

    shell(const shell &) = delete;
    shell& operator=(const shell &) = delete;

    shell(shell && lhs) noexcept 
        : buffer_(std::move(lhs.buffer_)),
          input_(std::move(lhs.input_)),
          argc_(boost::exchange(lhs.argc_, 0)),
          argv_(boost::exchange(lhs.argv_, nullptr))
#if defined(BOOST_PROCESS_V2_POSIX)
        , free_argv_(boost::exchange(lhs.free_argv_, nullptr))
#endif
    {
    }
    shell& operator=(shell && lhs) noexcept
    {
        shell tmp(std::move(*this));
        buffer_ = std::move(lhs.buffer_);
        input_ = std::move(lhs.input_);
        argc_  = boost::exchange(lhs.argc_, 0);
        argv_ = boost::exchange(lhs.argv_, nullptr);
#if defined(BOOST_PROCESS_V2_POSIX)
        free_argv_ = boost::exchange(lhs.free_argv_, nullptr);
#endif
        return *this;
    }

    // the length of the parsed shell, including the executable
    int argc() const { return argc_; }
    char_type** argv() const { return argv_; }
        
    char_type** begin() const {return argv();}
    char_type** end()   const {return argv() + argc();}

    bool empty() const {return argc() == 0;}
    std::size_t size() const {return static_cast<std::size_t>(argc()); }
    /// Native representation of the arguments to be used - excluding the executable
    BOOST_PROCESS_V2_DECL args_type args() const;
    template<typename Environment = environment::current_view>
    filesystem::path exe(Environment && env = environment::current()) const
    {
        if (argc() == 0)
            return "";
        else
            return environment::find_executable(0[argv()], std::forward<Environment>(env)); 
    }
    BOOST_PROCESS_V2_DECL ~shell();

  private:

    friend struct make_cmd_shell_;

    BOOST_PROCESS_V2_DECL void parse_();
    
    // storage in case we need a conversion
    std::basic_string<char_type> buffer_;
    basic_cstring_ref<char_type> input_{buffer_}; 
    // impl details
    int argc_ = 0;
    char_type  ** argv_ = nullptr;

#if defined(BOOST_PROCESS_V2_POSIX)
    void(*free_argv_)(int, char **);
#endif
    
};

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_ERROR_HPP
