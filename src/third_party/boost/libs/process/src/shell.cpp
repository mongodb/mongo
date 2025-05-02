// Copyright (c) 2022 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/process/v2/detail/config.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <boost/asio/windows/basic_object_handle.hpp>
#endif


#include <boost/process/v2/detail/last_error.hpp>
#include <boost/process/v2/detail/throw_error.hpp>
#include <boost/process/v2/detail/config.hpp>
#include <boost/process/v2/error.hpp>
#include <boost/process/v2/shell.hpp>

#if defined(BOOST_PROCESS_V2_WINDOWS)
#include <windows.h>
#include <shellapi.h>
#elif !defined(__OpenBSD__) && !defined(__ANDROID__)
#include <wordexp.h>
#endif

BOOST_PROCESS_V2_BEGIN_NAMESPACE

#if defined(BOOST_PROCESS_V2_WINDOWS)
BOOST_PROCESS_V2_DECL const error_category& get_shell_category()
{
    return system_category();
}
#elif !defined(__OpenBSD__) && !defined(__ANDROID__)

struct shell_category_t final : public error_category
{
    shell_category_t() : error_category(0xDAF1u) {}

    const char* name() const noexcept
    {
        return "process.v2.shell";
    }
    std::string message(int value) const
    {
        switch (value)
        {
        case WRDE_BADCHAR:
            return "Illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }.";
        case WRDE_BADVAL:
            return "An undefined shell variable was referenced, and the WRDE_UNDEF flag told us to consider this an error.";
        case WRDE_CMDSUB:
            return "Command substitution occurred, and the WRDE_NOCMD flag told us to consider this an error.";
        case WRDE_NOSPACE:
            return "Out of memory.";
        case WRDE_SYNTAX:
            return "Shell syntax error, such as unbalanced parentheses or unmatched quotes.";
        default:
            return "process.v2.wordexp error";
        }
    }
};

BOOST_PROCESS_V2_DECL const error_category& get_shell_category()
{
    static shell_category_t instance;
    return instance;
}

#else

const error_category& get_shell_category()
{
    return system_category();
}

#endif

#if defined (BOOST_PROCESS_V2_WINDOWS)

void shell::parse_()
{
    argv_ = ::CommandLineToArgvW(input_.c_str(), &argc_);
    if (argv_ == nullptr)
    {
        error_code ec;
        BOOST_PROCESS_V2_ASSIGN_LAST_ERROR(ec);
        throw system_error(ec, "shell::parse");
    }
}

shell::~shell()
{
    if (argv_ != nullptr)
        LocalFree(argv_);
}

auto shell::args() const-> args_type
{
    return input_.c_str();
}

#elif !defined(__OpenBSD__) && !defined(__ANDROID__)

void shell::parse_()
{
    wordexp_t we{};
    auto cd = wordexp(input_.c_str(), &we, WRDE_NOCMD);

    if (cd != 0)
        detail::throw_error(error_code(cd, get_shell_category()), "shell::parse");
    else
    {
        argc_ = static_cast<int>(we.we_wordc);
        argv_ = we.we_wordv;
    }

    free_argv_ = +[](int argc, char ** argv)
    {
        wordexp_t we{
                .we_wordc = static_cast<std::size_t>(argc),
                .we_wordv = argv,
                .we_offs = 0
        };
        wordfree(&we);
    };
}

shell::~shell()
{
    if (argv_ != nullptr && free_argv_ != nullptr)
        free_argv_(argc_, argv_);
}

auto shell::args() const -> args_type
{
    if (argc() == 0)
    {
        static const char * helper = nullptr;
        return &helper;
    }
    else
        return const_cast<const char**>(argv());
}

#else

void shell::parse_()
{
    error_code ec;
    BOOST_PROCESS_V2_ASSIGN_EC(ec, ENOTSUP, system_category());
    throw system_error(ec, "shell::parse");
}

shell::~shell() = default;

auto shell::args() const -> args_type
{
    return nullptr;
}

#endif

BOOST_PROCESS_V2_END_NAMESPACE
