// Support for interoperability between Boost.System and <system_error>
//
// Copyright 2018 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <system_error>
#include <map>
#include <memory>

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

    explicit std_category( boost::system::error_category const * pc ): pc_( pc )
    {
    }

    virtual const char * name() const BOOST_NOEXCEPT
    {
        return pc_->name();
    }

    virtual std::string message( int ev ) const
    {
        return pc_->message( ev );
    }

    virtual std::error_condition default_error_condition( int ev ) const BOOST_NOEXCEPT
    {
        return pc_->default_error_condition( ev );
    }

    virtual bool equivalent( int code, const std::error_condition & condition ) const BOOST_NOEXCEPT;
    virtual bool equivalent( const std::error_code & code, int condition ) const BOOST_NOEXCEPT;
};

inline std::error_category const & to_std_category( boost::system::error_category const & cat )
{
    typedef std::map< boost::system::error_category const *, std::unique_ptr<std_category> > map_type;

    static map_type map_;

    map_type::iterator i = map_.find( &cat );

    if( i == map_.end() )
    {
        std::unique_ptr<std_category> p( new std_category( &cat ) );

        std::pair<map_type::iterator, bool> r = map_.insert( map_type::value_type( &cat, std::move( p ) ) );

        i = r.first;
    }

    return *i->second;
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
