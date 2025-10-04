#ifndef BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_HPP_INCLUDED

//  Copyright Beman Dawes 2006, 2007
//  Copyright Christoper Kohlhoff 2007
//  Copyright Peter Dimov 2017, 2018
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/config.hpp>
#include <boost/cstdint.hpp>
#include <boost/config.hpp>
#include <boost/config/workaround.hpp>
#include <string>
#include <functional>
#include <cstddef>
#include <system_error>
#include <atomic>

namespace boost
{

namespace system
{

class error_category;
class error_code;
class error_condition;

std::size_t hash_value( error_code const & ec );

namespace detail
{

BOOST_SYSTEM_CONSTEXPR bool failed_impl( int ev, error_category const & cat );

class std_category;

} // namespace detail

#if ( defined( BOOST_GCC ) && BOOST_GCC >= 40600 ) || defined( BOOST_CLANG )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#if defined(BOOST_MSVC) && BOOST_MSVC < 1900
#pragma warning(push)
#pragma warning(disable: 4351) //  new behavior: elements of array will be default initialized
#endif

class BOOST_SYMBOL_VISIBLE error_category
{
private:

    friend std::size_t hash_value( error_code const & ec );
    friend BOOST_SYSTEM_CONSTEXPR bool detail::failed_impl( int ev, error_category const & cat );

    friend class error_code;
    friend class error_condition;

public:

    error_category( error_category const & ) = delete;
    error_category& operator=( error_category const & ) = delete;

private:

    boost::ulong_long_type id_;

    static std::size_t const stdcat_size_ = 4 * sizeof( void const* );

    union
    {
        mutable unsigned char stdcat_[ stdcat_size_ ];
        void const* stdcat_align_;
    };

    mutable std::atomic< unsigned > sc_init_;

protected:

    ~error_category() = default;

    constexpr error_category() noexcept: id_( 0 ), stdcat_(), sc_init_()
    {
    }

    explicit constexpr error_category( boost::ulong_long_type id ) noexcept: id_( id ), stdcat_(), sc_init_()
    {
    }

public:

    virtual const char * name() const noexcept = 0;

    virtual error_condition default_error_condition( int ev ) const noexcept;
    virtual bool equivalent( int code, const error_condition & condition ) const noexcept;
    virtual bool equivalent( const error_code & code, int condition ) const noexcept;

    virtual std::string message( int ev ) const = 0;
    virtual char const * message( int ev, char * buffer, std::size_t len ) const noexcept;

    virtual bool failed( int ev ) const noexcept
    {
        return ev != 0;
    }

    friend BOOST_SYSTEM_CONSTEXPR bool operator==( error_category const & lhs, error_category const & rhs ) noexcept
    {
        return rhs.id_ == 0? &lhs == &rhs: lhs.id_ == rhs.id_;
    }

    friend BOOST_SYSTEM_CONSTEXPR bool operator!=( error_category const & lhs, error_category const & rhs ) noexcept
    {
        return !( lhs == rhs );
    }

    friend BOOST_SYSTEM_CONSTEXPR bool operator<( error_category const & lhs, error_category const & rhs ) noexcept
    {
        if( lhs.id_ < rhs.id_ )
        {
            return true;
        }

        if( lhs.id_ > rhs.id_ )
        {
            return false;
        }

        if( rhs.id_ != 0 )
        {
            return false; // equal
        }

        return std::less<error_category const *>()( &lhs, &rhs );
    }

    void init_stdcat() const;

# if defined(__SUNPRO_CC) // trailing __global is not supported
    operator std::error_category const & () const;
# else
    operator std::error_category const & () const BOOST_SYMBOL_VISIBLE;
# endif
};

#if defined(BOOST_MSVC) && BOOST_MSVC < 1900
#pragma warning(pop)
#endif

#if ( defined( BOOST_GCC ) && BOOST_GCC >= 40600 ) || defined( BOOST_CLANG )
#pragma GCC diagnostic pop
#endif

namespace detail
{

static const boost::ulong_long_type generic_category_id = ( boost::ulong_long_type( 0xB2AB117A ) << 32 ) + 0x257EDFD0;
static const boost::ulong_long_type system_category_id = generic_category_id + 1;
static const boost::ulong_long_type interop_category_id = generic_category_id + 2;

BOOST_SYSTEM_CONSTEXPR inline bool failed_impl( int ev, error_category const & cat )
{
    if( cat.id_ == system_category_id || cat.id_ == generic_category_id )
    {
        return ev != 0;
    }
    else
    {
        return cat.failed( ev );
    }
}

} // namespace detail

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_ERROR_CATEGORY_HPP_INCLUDED
