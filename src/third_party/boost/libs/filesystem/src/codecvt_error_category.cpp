//  codecvt_error_category implementation file  ----------------------------------------//

//  Copyright 2009 Beman Dawes
//  Copyright 2022 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt)

//  Library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/config/warning_disable.hpp>

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/detail/path_traits.hpp>
#include <boost/system/error_category.hpp>
#include <locale>
#include <string>

#include "private_config.hpp"

#include <boost/filesystem/detail/header.hpp> // must be the last #include

//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {

namespace {

#if (defined(BOOST_GCC) && BOOST_GCC >= 40600) || defined(BOOST_CLANG)
#pragma GCC diagnostic push
// '(anonymous namespace)::codecvt_error_cat' has virtual functions but non-virtual destructor
// This is not a problem as instances of codecvt_error_cat are never destroyed through a pointer to base.
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

class codecvt_error_cat final :
    public boost::system::error_category
{
public:
    // clang up to version 3.8 requires a user-defined default constructor in order to be able to declare a static constant of the error category.
    BOOST_SYSTEM_CONSTEXPR codecvt_error_cat() noexcept {}
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

const char* codecvt_error_cat::name() const noexcept
{
    return "codecvt";
}

std::string codecvt_error_cat::message(int ev) const
{
    std::string str;
    switch (ev)
    {
    case std::codecvt_base::ok:
        str = "ok";
        break;
    case std::codecvt_base::partial:
        str = "partial";
        break;
    case std::codecvt_base::error:
        str = "error";
        break;
    case std::codecvt_base::noconv:
        str = "noconv";
        break;
    default:
        str = "unknown error";
        break;
    }
    return str;
}

#if (defined(BOOST_GCC) && BOOST_GCC >= 40600) || defined(BOOST_CLANG)
#pragma GCC diagnostic pop
#endif

} // unnamed namespace

BOOST_FILESYSTEM_DECL boost::system::error_category const& codecvt_error_category() noexcept
{
    static
#if defined(BOOST_SYSTEM_HAS_CONSTEXPR)
        constexpr
#else
        const
#endif
        codecvt_error_cat codecvt_error_cat_const;
    return codecvt_error_cat_const;
}

// Try to initialize the error category instance as early as possible to make sure it is
// available during global deinitialization stage. For MSVC, codecvt_error_category() will
// be called early by MSVC-specific initialization routine in path.cpp.
#if !defined(BOOST_SYSTEM_HAS_CONSTEXPR) && !defined(_MSC_VER)

namespace {

struct codecvt_error_category_initializer
{
    codecvt_error_category_initializer() { boost::filesystem::codecvt_error_category(); }
};

BOOST_FILESYSTEM_INIT_PRIORITY(BOOST_FILESYSTEM_PATH_GLOBALS_INIT_PRIORITY) BOOST_ATTRIBUTE_UNUSED BOOST_FILESYSTEM_ATTRIBUTE_RETAIN
const codecvt_error_category_initializer g_codecvt_error_category_initializer;

} // namespace

#endif // !defined(BOOST_SYSTEM_HAS_CONSTEXPR) && !defined(_MSC_VER)

} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>
