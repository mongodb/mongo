#ifndef BOOST_SYSTEM_DETAIL_TO_STD_CATEGORY_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_TO_STD_CATEGORY_HPP_INCLUDED

// Support for interoperability between Boost.System and <system_error>
//
// Copyright 2018 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/system/detail/error_condition.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/system/detail/generic_category.hpp>
#include <system_error>
#include <map>
#include <memory>
#include <mutex>

//

namespace boost
{

namespace system
{

namespace detail
{

class BOOST_SYMBOL_VISIBLE std_category: public std::error_category
{
private:

    boost::system::error_category const * pc_;

public:

    explicit std_category( boost::system::error_category const * pc, unsigned id ): pc_( pc )
    {
        if( id != 0 )
        {
#if defined(_MSC_VER) && defined(_CPPLIB_VER) && _MSC_VER >= 1900 && _MSC_VER < 2000

            // Poking into the protected _Addr member of std::error_category
            // is not a particularly good programming practice, but what can
            // you do

            _Addr = id;

#endif
        }
    }

    const char * name() const BOOST_NOEXCEPT BOOST_OVERRIDE
    {
        return pc_->name();
    }

    std::string message( int ev ) const BOOST_OVERRIDE
    {
        return pc_->message( ev );
    }

    std::error_condition default_error_condition( int ev ) const BOOST_NOEXCEPT BOOST_OVERRIDE
    {
        return pc_->default_error_condition( ev );
    }

    bool equivalent( int code, const std::error_condition & condition ) const BOOST_NOEXCEPT BOOST_OVERRIDE;
    bool equivalent( const std::error_code & code, int condition ) const BOOST_NOEXCEPT BOOST_OVERRIDE;
};

#if !defined(__SUNPRO_CC) // trailing __global is not supported
inline std::error_category const & to_std_category( boost::system::error_category const & cat ) BOOST_SYMBOL_VISIBLE;
#endif

struct cat_ptr_less
{
    bool operator()( boost::system::error_category const * p1, boost::system::error_category const * p2 ) const BOOST_NOEXCEPT
    {
        return *p1 < *p2;
    }
};

inline std::error_category const & to_std_category( boost::system::error_category const & cat )
{
    if( cat.id_ == boost::system::detail::system_category_id )
    {
        static const std_category system_instance( &cat, 0x1F4D7 );
        return system_instance;
    }
    else if( cat.id_ == boost::system::detail::generic_category_id )
    {
        static const std_category generic_instance( &cat, 0x1F4D3 );
        return generic_instance;
    }
    else
    {
        typedef std::map< boost::system::error_category const *, std::unique_ptr<std_category>, cat_ptr_less > map_type;

        static map_type map_;
        static std::mutex map_mx_;

        std::lock_guard<std::mutex> guard( map_mx_ );

        map_type::iterator i = map_.find( &cat );

        if( i == map_.end() )
        {
            std::unique_ptr<std_category> p( new std_category( &cat, 0 ) );

            std::pair<map_type::iterator, bool> r = map_.insert( map_type::value_type( &cat, std::move( p ) ) );

            i = r.first;
        }

        return *i->second;
    }
}

inline bool std_category::equivalent( int code, const std::error_condition & condition ) const BOOST_NOEXCEPT
{
    if( condition.category() == *this )
    {
        boost::system::error_condition bn( condition.value(), *pc_ );
        return pc_->equivalent( code, bn );
    }
    else if( condition.category() == std::generic_category() || condition.category() == boost::system::generic_category() )
    {
        boost::system::error_condition bn( condition.value(), boost::system::generic_category() );
        return pc_->equivalent( code, bn );
    }

#ifndef BOOST_NO_RTTI

    else if( std_category const* pc2 = dynamic_cast< std_category const* >( &condition.category() ) )
    {
        boost::system::error_condition bn( condition.value(), *pc2->pc_ );
        return pc_->equivalent( code, bn );
    }

#endif

    else
    {
        return default_error_condition( code ) == condition;
    }
}

inline bool std_category::equivalent( const std::error_code & code, int condition ) const BOOST_NOEXCEPT
{
    if( code.category() == *this )
    {
        boost::system::error_code bc( code.value(), *pc_ );
        return pc_->equivalent( bc, condition );
    }
    else if( code.category() == std::generic_category() || code.category() == boost::system::generic_category() )
    {
        boost::system::error_code bc( code.value(), boost::system::generic_category() );
        return pc_->equivalent( bc, condition );
    }

#ifndef BOOST_NO_RTTI

    else if( std_category const* pc2 = dynamic_cast< std_category const* >( &code.category() ) )
    {
        boost::system::error_code bc( code.value(), *pc2->pc_ );
        return pc_->equivalent( bc, condition );
    }
#endif

    else if( *pc_ == boost::system::generic_category() )
    {
        return std::generic_category().equivalent( code, condition );
    }
    else
    {
        return false;
    }
}

} // namespace detail

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_TO_STD_CATEGORY_HPP_INCLUDED
