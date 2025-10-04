#ifndef BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_IMPL_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_IMPL_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_condition.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/snprintf.hpp>
#include <boost/system/detail/config.hpp>
#include <boost/config.hpp>
#include <string>
#include <cstring>

namespace boost
{
namespace system
{

// error_category default implementation

inline error_condition error_category::default_error_condition( int ev ) const noexcept
{
    return error_condition( ev, *this );
}

inline bool error_category::equivalent( int code, const error_condition & condition ) const noexcept
{
    return default_error_condition( code ) == condition;
}

inline bool error_category::equivalent( const error_code & code, int condition ) const noexcept
{
    return code.equals( condition, *this );
}

inline char const * error_category::message( int ev, char * buffer, std::size_t len ) const noexcept
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
        detail::snprintf( buffer, len, "%s", this->message( ev ).c_str() );
        return buffer;
    }
#if !defined(BOOST_NO_EXCEPTIONS)
    catch( ... )
    {
        detail::snprintf( buffer, len, "No message text available for error %d", ev );
        return buffer;
    }
#endif
}

} // namespace system
} // namespace boost

// interoperability with std::error_code, std::error_condition

#include <boost/system/detail/std_category_impl.hpp>
#include <boost/system/detail/mutex.hpp>
#include <new>

namespace boost
{
namespace system
{

inline void error_category::init_stdcat() const
{
    static_assert( sizeof( stdcat_ ) >= sizeof( boost::system::detail::std_category ), "sizeof(stdcat_) is not enough for std_category" );

#if defined(BOOST_MSVC) && BOOST_MSVC < 1900
    // no alignof
#else

    static_assert( alignof( decltype(stdcat_align_) ) >= alignof( boost::system::detail::std_category ), "alignof(stdcat_) is not enough for std_category" );

#endif

    // detail::mutex has a constexpr default constructor,
    // and therefore guarantees static initialization, on
    // everything except VS 2013 (msvc-12.0)

    static system::detail::mutex mx_;

    system::detail::lock_guard<system::detail::mutex> lk( mx_ );

    if( sc_init_.load( std::memory_order_acquire ) == 0 )
    {
        ::new( static_cast<void*>( stdcat_ ) ) boost::system::detail::std_category( this, system::detail::id_wrapper<0>() );
        sc_init_.store( 1, std::memory_order_release );
    }
}

#if defined( BOOST_GCC ) && BOOST_GCC >= 40800 && BOOST_GCC < 70000
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

inline BOOST_NOINLINE error_category::operator std::error_category const & () const
{
    if( id_ == detail::generic_category_id )
    {
// This condition must be the same as the one in error_condition.hpp
#if defined(BOOST_SYSTEM_AVOID_STD_GENERIC_CATEGORY)

        static const boost::system::detail::std_category generic_instance( this, system::detail::id_wrapper<0x1F4D3>() );
        return generic_instance;

#else

        return std::generic_category();

#endif
    }

    if( id_ == detail::system_category_id )
    {
// This condition must be the same as the one in error_code.hpp
#if defined(BOOST_SYSTEM_AVOID_STD_SYSTEM_CATEGORY)

        static const boost::system::detail::std_category system_instance( this, system::detail::id_wrapper<0x1F4D7>() );
        return system_instance;

#else

        return std::system_category();

#endif
    }

    if( sc_init_.load( std::memory_order_acquire ) == 0 )
    {
        init_stdcat();
    }

    return *static_cast<boost::system::detail::std_category const*>( static_cast<void const*>( stdcat_ ) );
}

#if defined( BOOST_GCC ) && BOOST_GCC >= 40800 && BOOST_GCC < 70000
#pragma GCC diagnostic pop
#endif

} // namespace system
} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_IMPL_HPP_INCLUDED
