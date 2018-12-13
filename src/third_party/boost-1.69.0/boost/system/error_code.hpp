#ifndef BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED
#define BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/api_config.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <ostream>
#include <string>
#include <functional>
#include <cstring>

// TODO: undef these macros if not already defined
#include <boost/cerrno.hpp>

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)
# include <system_error>
#endif

#if !defined(BOOST_POSIX_API) && !defined(BOOST_WINDOWS_API)
#  error BOOST_POSIX_API or BOOST_WINDOWS_API must be defined
#endif

namespace boost
{

namespace system
{

class error_code;         // values defined by the operating system
class error_condition;    // portable generic values defined below, but ultimately
                          // based on the POSIX standard

// "Concept" helpers

template<class T> struct is_error_code_enum
{
    static const bool value = false;
};

template<class T> struct is_error_condition_enum
{
    static const bool value = false;
};

// Generic error_conditions

namespace errc
{

enum errc_t
{
    success = 0,
    address_family_not_supported = EAFNOSUPPORT,
    address_in_use = EADDRINUSE,
    address_not_available = EADDRNOTAVAIL,
    already_connected = EISCONN,
    argument_list_too_long = E2BIG,
    argument_out_of_domain = EDOM,
    bad_address = EFAULT,
    bad_file_descriptor = EBADF,
    bad_message = EBADMSG,
    broken_pipe = EPIPE,
    connection_aborted = ECONNABORTED,
    connection_already_in_progress = EALREADY,
    connection_refused = ECONNREFUSED,
    connection_reset = ECONNRESET,
    cross_device_link = EXDEV,
    destination_address_required = EDESTADDRREQ,
    device_or_resource_busy = EBUSY,
    directory_not_empty = ENOTEMPTY,
    executable_format_error = ENOEXEC,
    file_exists = EEXIST,
    file_too_large = EFBIG,
    filename_too_long = ENAMETOOLONG,
    function_not_supported = ENOSYS,
    host_unreachable = EHOSTUNREACH,
    identifier_removed = EIDRM,
    illegal_byte_sequence = EILSEQ,
    inappropriate_io_control_operation = ENOTTY,
    interrupted = EINTR,
    invalid_argument = EINVAL,
    invalid_seek = ESPIPE,
    io_error = EIO,
    is_a_directory = EISDIR,
    message_size = EMSGSIZE,
    network_down = ENETDOWN,
    network_reset = ENETRESET,
    network_unreachable = ENETUNREACH,
    no_buffer_space = ENOBUFS,
    no_child_process = ECHILD,
    no_link = ENOLINK,
    no_lock_available = ENOLCK,
    no_message_available = ENODATA,
    no_message = ENOMSG,
    no_protocol_option = ENOPROTOOPT,
    no_space_on_device = ENOSPC,
    no_stream_resources = ENOSR,
    no_such_device_or_address = ENXIO,
    no_such_device = ENODEV,
    no_such_file_or_directory = ENOENT,
    no_such_process = ESRCH,
    not_a_directory = ENOTDIR,
    not_a_socket = ENOTSOCK,
    not_a_stream = ENOSTR,
    not_connected = ENOTCONN,
    not_enough_memory = ENOMEM,
    not_supported = ENOTSUP,
    operation_canceled = ECANCELED,
    operation_in_progress = EINPROGRESS,
    operation_not_permitted = EPERM,
    operation_not_supported = EOPNOTSUPP,
    operation_would_block = EWOULDBLOCK,
    owner_dead = EOWNERDEAD,
    permission_denied = EACCES,
    protocol_error = EPROTO,
    protocol_not_supported = EPROTONOSUPPORT,
    read_only_file_system = EROFS,
    resource_deadlock_would_occur = EDEADLK,
    resource_unavailable_try_again = EAGAIN,
    result_out_of_range = ERANGE,
    state_not_recoverable = ENOTRECOVERABLE,
    stream_timeout = ETIME,
    text_file_busy = ETXTBSY,
    timed_out = ETIMEDOUT,
    too_many_files_open_in_system = ENFILE,
    too_many_files_open = EMFILE,
    too_many_links = EMLINK,
    too_many_symbolic_link_levels = ELOOP,
    value_too_large = EOVERFLOW,
    wrong_protocol_type = EPROTOTYPE
};

} // namespace errc

#ifdef BOOST_SYSTEM_ENABLE_DEPRECATED

namespace posix = errc;
namespace posix_error = errc;

#endif

template<> struct is_error_condition_enum<errc::errc_t>
{
    static const bool value = true;
};

// class error_category

#ifdef BOOST_MSVC
#pragma warning( push )
// 'this' : used in base member initializer list
#pragma warning( disable: 4355 )
#endif

std::size_t hash_value( error_code const & ec );

class BOOST_SYMBOL_VISIBLE error_category
{
private:

    friend std::size_t hash_value( error_code const & ec );

#if !defined(BOOST_NO_CXX11_DELETED_FUNCTIONS)
public:

    error_category( error_category const & ) = delete;
    error_category& operator=( error_category const & ) = delete;

#else
private:

    error_category( error_category const & );
    error_category& operator=( error_category const & );

#endif

private:

    boost::ulong_long_type id_;

protected:

#if !defined(BOOST_NO_CXX11_DEFAULTED_FUNCTIONS) && !defined(BOOST_NO_CXX11_NON_PUBLIC_DEFAULTED_FUNCTIONS)

    ~error_category() = default;

#else

    // We'd like to make the destructor protected, to make code that deletes
    // an error_category* not compile; unfortunately, doing the below makes
    // the destructor user-provided and hence breaks use after main, as the
    // categories may get destroyed before code that uses them

    // ~error_category() {}

#endif

    BOOST_SYSTEM_CONSTEXPR error_category() BOOST_NOEXCEPT: id_( 0 )
    {
    }

    explicit BOOST_SYSTEM_CONSTEXPR error_category( boost::ulong_long_type id ) BOOST_NOEXCEPT: id_( id )
    {
    }

public:

    virtual const char * name() const BOOST_NOEXCEPT = 0;

    virtual error_condition default_error_condition( int ev ) const BOOST_NOEXCEPT;
    virtual bool equivalent( int code, const error_condition & condition ) const BOOST_NOEXCEPT;
    virtual bool equivalent( const error_code & code, int condition ) const BOOST_NOEXCEPT;

    virtual std::string message( int ev ) const = 0;
    virtual char const * message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT;

    virtual bool failed( int ev ) const BOOST_NOEXCEPT;

    BOOST_SYSTEM_CONSTEXPR bool operator==( const error_category & rhs ) const BOOST_NOEXCEPT
    {
        return rhs.id_ == 0? this == &rhs: id_ == rhs.id_;
    }

    BOOST_SYSTEM_CONSTEXPR bool operator!=( const error_category & rhs ) const BOOST_NOEXCEPT
    {
        return !( *this == rhs );
    }

    BOOST_SYSTEM_CONSTEXPR bool operator<( const error_category & rhs ) const BOOST_NOEXCEPT
    {
        if( id_ < rhs.id_ )
        {
            return true;
        }

        if( id_ > rhs.id_ )
        {
            return false;
        }

        if( rhs.id_ != 0 )
        {
            return false; // equal
        }

        return std::less<error_category const *>()( this, &rhs );
    }

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

    operator std::error_category const & () const;

#endif
};

#ifdef BOOST_MSVC
#pragma warning( pop )
#endif

// predefined error categories

namespace detail
{

class BOOST_SYMBOL_VISIBLE generic_error_category: public error_category
{
public:

    // clang++ 3.8 and below: initialization of const object
    // requires a user-provided default constructor
    BOOST_SYSTEM_CONSTEXPR generic_error_category() BOOST_NOEXCEPT:
        error_category( ( boost::ulong_long_type( 0xB2AB117A ) << 32 ) + 0x257EDF0D )
    {
    }

    const char * name() const BOOST_NOEXCEPT
    {
        return "generic";
    }

    std::string message( int ev ) const;
    char const * message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT;
};

class BOOST_SYMBOL_VISIBLE system_error_category: public error_category
{
public:

    BOOST_SYSTEM_CONSTEXPR system_error_category() BOOST_NOEXCEPT:
        error_category( ( boost::ulong_long_type( 0x8FAFD21E ) << 32 ) + 0x25C5E09B )
    {
    }

    const char * name() const BOOST_NOEXCEPT
    {
        return "system";
    }

    error_condition default_error_condition( int ev ) const BOOST_NOEXCEPT;

    std::string message( int ev ) const;
    char const * message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT;
};

} // namespace detail

// generic_category(), system_category()

#if defined(BOOST_SYSTEM_HAS_CONSTEXPR)

namespace detail
{

template<class T> struct cat_holder
{
    BOOST_SYSTEM_REQUIRE_CONST_INIT static constexpr system_error_category system_category_instance{};
    BOOST_SYSTEM_REQUIRE_CONST_INIT static constexpr generic_error_category generic_category_instance{};
};

template<class T> BOOST_SYSTEM_REQUIRE_CONST_INIT constexpr system_error_category cat_holder<T>::system_category_instance;
template<class T> BOOST_SYSTEM_REQUIRE_CONST_INIT constexpr generic_error_category cat_holder<T>::generic_category_instance;

} // namespace detail

constexpr error_category const & system_category() BOOST_NOEXCEPT
{
    return detail::cat_holder<void>::system_category_instance;
}

constexpr error_category const & generic_category() BOOST_NOEXCEPT
{
    return detail::cat_holder<void>::generic_category_instance;
}

#else // #if defined(BOOST_SYSTEM_HAS_CONSTEXPR)

inline error_category const & system_category() BOOST_NOEXCEPT
{
    static const detail::system_error_category system_category_instance;
    return system_category_instance;
}

inline error_category const & generic_category() BOOST_NOEXCEPT
{
    static const detail::generic_error_category generic_category_instance;
    return generic_category_instance;
}

#endif // #if defined(BOOST_SYSTEM_HAS_CONSTEXPR)

// deprecated synonyms

#ifdef BOOST_SYSTEM_ENABLE_DEPRECATED

inline const error_category & get_system_category() { return system_category(); }
inline const error_category & get_generic_category() { return generic_category(); }
inline const error_category & get_posix_category() { return generic_category(); }
static const error_category & posix_category BOOST_ATTRIBUTE_UNUSED = generic_category();
static const error_category & errno_ecat BOOST_ATTRIBUTE_UNUSED = generic_category();
static const error_category & native_ecat BOOST_ATTRIBUTE_UNUSED = system_category();

#endif

// enable_if

namespace detail
{

template<bool C, class T = void> struct enable_if
{
    typedef T type;
};

template<class T> struct enable_if<false, T>
{
};

// failed_impl

#if !defined(BOOST_SYSTEM_HAS_CONSTEXPR)

inline bool failed_impl( int ev, error_category const & cat )
{
    return cat.failed( ev );
}

#else

BOOST_SYSTEM_CONSTEXPR inline bool failed_impl( int ev, error_category const & cat )
{
    if( cat == system_category() || cat == generic_category() )
    {
        return ev != 0;
    }
    else
    {
        return cat.failed( ev );
    }
}

#endif

} // namespace detail

// class error_condition

// error_conditions are portable, error_codes are system or library specific

class error_condition
{
private:

    int val_;
    bool failed_;
    error_category const * cat_;

public:

    // constructors:

    BOOST_SYSTEM_CONSTEXPR error_condition() BOOST_NOEXCEPT:
        val_( 0 ), failed_( false ), cat_( &generic_category() )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_condition( int val, const error_category & cat ) BOOST_NOEXCEPT:
        val_( val ), failed_( detail::failed_impl( val, cat ) ), cat_( &cat )
    {
    }

    template<class ErrorConditionEnum> BOOST_SYSTEM_CONSTEXPR error_condition( ErrorConditionEnum e,
        typename detail::enable_if<is_error_condition_enum<ErrorConditionEnum>::value>::type* = 0) BOOST_NOEXCEPT
    {
        *this = make_error_condition( e );
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) BOOST_NOEXCEPT
    {
        val_ = val;
        failed_ = detail::failed_impl( val, cat );
        cat_ = &cat;
    }

    template<typename ErrorConditionEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_condition_enum<ErrorConditionEnum>::value, error_condition>::type &
        operator=( ErrorConditionEnum val ) BOOST_NOEXCEPT
    {
        *this = make_error_condition( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() BOOST_NOEXCEPT
    {
        val_ = 0;
        failed_ = false;
        cat_ = &generic_category();
    }

    // observers:

    BOOST_SYSTEM_CONSTEXPR int value() const BOOST_NOEXCEPT
    {
        return val_;
    }

    BOOST_SYSTEM_CONSTEXPR const error_category & category() const BOOST_NOEXCEPT
    {
        return *cat_;
    }

    std::string message() const
    {
        return cat_->message( value() );
    }

    char const * message( char * buffer, std::size_t len ) const BOOST_NOEXCEPT
    {
        return cat_->message( value(), buffer, len );
    }

    BOOST_SYSTEM_CONSTEXPR bool failed() const BOOST_NOEXCEPT
    {
        return failed_;
    }

#if !defined(BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS)

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const BOOST_NOEXCEPT  // true if error
    {
        return failed_;
    }

#else

    typedef void (*unspecified_bool_type)();
    static void unspecified_bool_true() {}

    BOOST_SYSTEM_CONSTEXPR operator unspecified_bool_type() const BOOST_NOEXCEPT  // true if error
    {
        return failed_? unspecified_bool_true: 0;
    }

    BOOST_SYSTEM_CONSTEXPR bool operator!() const BOOST_NOEXCEPT  // true if no error
    {
        return !failed_;
    }

#endif

    // relationals:
    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
    {
        return lhs.val_ == rhs.val_ && *lhs.cat_ == *rhs.cat_;
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
    {
        return *lhs.cat_ < *rhs.cat_ || ( *lhs.cat_ == *rhs.cat_ && lhs.val_ < rhs.val_ );
    }

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

    operator std::error_condition () const
    {
        return std::error_condition( value(), category() );
    }

#endif
};

//  class error_code

//  We want error_code to be a value type that can be copied without slicing
//  and without requiring heap allocation, but we also want it to have
//  polymorphic behavior based on the error category. This is achieved by
//  abstract base class error_category supplying the polymorphic behavior,
//  and error_code containing a pointer to an object of a type derived
//  from error_category.

class error_code
{
private:

    int val_;
    bool failed_;
    const error_category * cat_;

public:

    // constructors:

    BOOST_SYSTEM_CONSTEXPR error_code() BOOST_NOEXCEPT:
        val_( 0 ), failed_( false ), cat_( &system_category() )
    {
    }

    BOOST_SYSTEM_CONSTEXPR error_code( int val, const error_category & cat ) BOOST_NOEXCEPT:
        val_( val ), failed_( detail::failed_impl( val, cat ) ), cat_( &cat )
    {
    }

    template<class ErrorCodeEnum> BOOST_SYSTEM_CONSTEXPR error_code( ErrorCodeEnum e,
        typename detail::enable_if<is_error_code_enum<ErrorCodeEnum>::value>::type* = 0 ) BOOST_NOEXCEPT
    {
        *this = make_error_code( e );
    }

    // modifiers:

    BOOST_SYSTEM_CONSTEXPR void assign( int val, const error_category & cat ) BOOST_NOEXCEPT
    {
        val_ = val;
        failed_ = detail::failed_impl( val, cat );
        cat_ = &cat;
    }

    template<typename ErrorCodeEnum>
        BOOST_SYSTEM_CONSTEXPR typename detail::enable_if<is_error_code_enum<ErrorCodeEnum>::value, error_code>::type &
        operator=( ErrorCodeEnum val ) BOOST_NOEXCEPT
    {
        *this = make_error_code( val );
        return *this;
    }

    BOOST_SYSTEM_CONSTEXPR void clear() BOOST_NOEXCEPT
    {
        val_ = 0;
        failed_ = false;
        cat_ = &system_category();
    }

    // observers:

    BOOST_SYSTEM_CONSTEXPR int value() const BOOST_NOEXCEPT
    {
        return val_;
    }

    BOOST_SYSTEM_CONSTEXPR const error_category & category() const BOOST_NOEXCEPT
    {
        return *cat_;
    }

    error_condition default_error_condition() const BOOST_NOEXCEPT
    {
        return cat_->default_error_condition( value() );
    }

    std::string message() const
    {
        return cat_->message( value() );
    }

    char const * message( char * buffer, std::size_t len ) const BOOST_NOEXCEPT
    {
        return cat_->message( value(), buffer, len );
    }

    BOOST_SYSTEM_CONSTEXPR bool failed() const BOOST_NOEXCEPT
    {
        return failed_;
    }

#if !defined(BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS)

    BOOST_SYSTEM_CONSTEXPR explicit operator bool() const BOOST_NOEXCEPT  // true if error
    {
        return failed_;
    }

#else

    typedef void (*unspecified_bool_type)();
    static void unspecified_bool_true() {}

    BOOST_SYSTEM_CONSTEXPR operator unspecified_bool_type() const  BOOST_NOEXCEPT // true if error
    {
        return failed_? unspecified_bool_true: 0;
    }

    BOOST_SYSTEM_CONSTEXPR bool operator!() const BOOST_NOEXCEPT // true if no error
    {
        return !failed_;
    }

#endif

    // relationals:

    //  the more symmetrical non-member syntax allows enum
    //  conversions work for both rhs and lhs.

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator==( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
    {
        return lhs.val_ == rhs.val_ && *lhs.cat_ == *rhs.cat_;
    }

    BOOST_SYSTEM_CONSTEXPR inline friend bool operator<( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
    {
        return *lhs.cat_ < *rhs.cat_ || ( *lhs.cat_ == *rhs.cat_ && lhs.val_ < rhs.val_ );
    }

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

    operator std::error_code () const
    {
        return std::error_code( value(), category() );
    }

#endif
};

}  // namespace system

// boost::throws()

namespace detail
{

//  Misuse of the error_code object is turned into a noisy failure by
//  poisoning the reference. This particular implementation doesn't
//  produce warnings or errors from popular compilers, is very efficient
//  (as determined by inspecting generated code), and does not suffer
//  from order of initialization problems. In practice, it also seems
//  cause user function error handling implementation errors to be detected
//  very early in the development cycle.

inline system::error_code* throws()
{
    // See github.com/boostorg/system/pull/12 by visigoth for why the return
    // is poisoned with nonzero rather than (0). A test, test_throws_usage(),
    // has been added to error_code_test.cpp, and as visigoth mentioned it
    // fails on clang for release builds with a return of 0 but works fine
    // with (1).
    // Since the undefined behavior sanitizer (-fsanitize=undefined) does not
    // allow a reference to be formed to the unaligned address of (1), we use
    // (8) instead.

    return reinterpret_cast<system::error_code*>(8);
}

} // namespace detail

inline system::error_code& throws()
{
    return *detail::throws();
}

// non-member functions of error_code and error_condition

namespace system
{

BOOST_SYSTEM_CONSTEXPR inline bool operator!=( const error_code & lhs, const error_code & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

BOOST_SYSTEM_CONSTEXPR inline bool operator!=( const error_condition & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

inline bool operator==( const error_code & code, const error_condition & condition ) BOOST_NOEXCEPT
{
    return code.category().equivalent( code.value(), condition ) || condition.category().equivalent( code, condition.value() );
}

inline bool operator!=( const error_code & lhs, const error_condition & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

inline bool operator==( const error_condition & condition, const error_code & code ) BOOST_NOEXCEPT
{
    return code.category().equivalent( code.value(), condition ) || condition.category().equivalent( code, condition.value() );
}

inline bool operator!=( const error_condition & lhs, const error_code & rhs ) BOOST_NOEXCEPT
{
    return !( lhs == rhs );
}

template <class charT, class traits>
    inline std::basic_ostream<charT,traits>&
    operator<< (std::basic_ostream<charT,traits>& os, error_code ec)
{
    os << ec.category().name() << ':' << ec.value();
    return os;
}

inline std::size_t hash_value( error_code const & ec )
{
    error_category const & cat = ec.category();

    boost::ulong_long_type id = cat.id_;

    if( id == 0 )
    {
        id = reinterpret_cast<boost::ulong_long_type>( &cat );
    }

    boost::ulong_long_type hv = ( boost::ulong_long_type( 0xCBF29CE4 ) << 32 ) + 0x84222325;
    boost::ulong_long_type const prime = ( boost::ulong_long_type( 0x00000100 ) << 32 ) + 0x000001B3;

    // id

    hv ^= id;
    hv *= prime;

    // value

    hv ^= static_cast<unsigned>( ec.value() );
    hv *= prime;

    return static_cast<std::size_t>( hv );
}

// make_* functions for errc::errc_t

namespace errc
{

// explicit conversion:
BOOST_SYSTEM_CONSTEXPR inline error_code make_error_code( errc_t e ) BOOST_NOEXCEPT
{
    return error_code( e, generic_category() );
}

// implicit conversion:
BOOST_SYSTEM_CONSTEXPR inline error_condition make_error_condition( errc_t e ) BOOST_NOEXCEPT
{
    return error_condition( e, generic_category() );
}

} // namespace errc

// error_category default implementation

inline error_condition error_category::default_error_condition( int ev ) const BOOST_NOEXCEPT
{
    return error_condition( ev, *this );
}

inline bool error_category::equivalent( int code, const error_condition & condition ) const BOOST_NOEXCEPT
{
    return default_error_condition( code ) == condition;
}

inline bool error_category::equivalent( const error_code & code, int condition ) const BOOST_NOEXCEPT
{
    return *this == code.category() && code.value() == condition;
}

inline char const * error_category::message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT
{
    if( len == 0 )
    {
        return buffer;
    }

    if( len == 1 )
    {
        buffer[0] = 0;
        return buffer;
    }

#if !defined(BOOST_NO_EXCEPTIONS)
    try
#endif
    {
        std::string m = this->message( ev );

# if defined( BOOST_MSVC )
#  pragma warning( push )
#  pragma warning( disable: 4996 )
# elif defined(__clang__) && defined(__has_warning)
#  pragma clang diagnostic push
#  if __has_warning("-Wdeprecated-declarations")
#   pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
# endif

        std::strncpy( buffer, m.c_str(), len - 1 );
        buffer[ len-1 ] = 0;

# if defined( BOOST_MSVC )
#  pragma warning( pop )
# elif defined(__clang__) && defined(__has_warning)
#  pragma clang diagnostic pop
# endif

        return buffer;
    }
#if !defined(BOOST_NO_EXCEPTIONS)
    catch( ... )
    {
        return "Message text unavailable";
    }
#endif
}

inline bool error_category::failed( int ev ) const BOOST_NOEXCEPT
{
    return ev != 0;
}

} // namespace system

} // namespace boost

// generic_error_category implementation

#include <boost/system/detail/generic_category.hpp>

inline std::string boost::system::detail::generic_error_category::message( int ev ) const
{
    return generic_error_category_message( ev );
}

inline char const * boost::system::detail::generic_error_category::message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT
{
    return generic_error_category_message( ev, buffer, len );
}

// system_error_category implementation

#if defined(BOOST_WINDOWS_API)

#include <boost/system/detail/system_category_win32.hpp>

inline boost::system::error_condition boost::system::detail::system_error_category::default_error_condition( int ev ) const BOOST_NOEXCEPT
{
    return system_category_default_error_condition_win32( ev );
}

inline std::string boost::system::detail::system_error_category::message( int ev ) const
{
    return system_category_message_win32( ev );
}

inline char const * boost::system::detail::system_error_category::message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT
{
    return system_category_message_win32( ev, buffer, len );
}

#else // #if defined(BOOST_WINDOWS_API)

#include <boost/system/detail/system_category_posix.hpp>

inline boost::system::error_condition boost::system::detail::system_error_category::default_error_condition( int ev ) const BOOST_NOEXCEPT
{
    return system_category_default_error_condition_posix( ev );
}

inline std::string boost::system::detail::system_error_category::message( int ev ) const
{
    return generic_error_category_message( ev );
}

inline char const * boost::system::detail::system_error_category::message( int ev, char * buffer, std::size_t len ) const BOOST_NOEXCEPT
{
    return generic_error_category_message( ev, buffer, len );
}

#endif // #if defined(BOOST_WINDOWS_API)

// interoperability with std::error_code, std::error_condition

#if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

#include <boost/system/detail/std_interoperability.hpp>

inline boost::system::error_category::operator std::error_category const & () const
{
    return boost::system::detail::to_std_category( *this );
}

#endif // #if defined(BOOST_SYSTEM_HAS_SYSTEM_ERROR)

#endif // BOOST_SYSTEM_ERROR_CODE_HPP_INCLUDED
