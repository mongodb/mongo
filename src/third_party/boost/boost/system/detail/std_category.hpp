#ifndef BOOST_SYSTEM_DETAIL_STD_CATEGORY_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_STD_CATEGORY_HPP_INCLUDED

// Support for interoperability between Boost.System and <system_error>
//
// Copyright 2018, 2021 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/error_category.hpp>
#include <boost/config.hpp>
#include <system_error>

//

namespace boost
{

namespace system
{

namespace detail
{

template<unsigned Id> struct id_wrapper {};

class BOOST_SYMBOL_VISIBLE std_category: public std::error_category
{
private:

    boost::system::error_category const * pc_;

public:

    boost::system::error_category const & original_category() const noexcept
    {
        return *pc_;
    }

public:

    template<unsigned Id>
    explicit std_category( boost::system::error_category const * pc, id_wrapper<Id> ): pc_( pc )
    {
#if defined(_MSC_VER) && defined(_CPPLIB_VER) && _MSC_VER >= 1900 && _MSC_VER < 2000

        // We used to assign to the protected _Addr member of std::error_category
        // here when Id != 0, but this should never happen now because this code
        // path is no longer used

        static_assert( Id == 0, "This constructor should only be called with Id == 0 under MS STL 14.0+" );

#endif
    }

    const char * name() const noexcept BOOST_OVERRIDE
    {
        return pc_->name();
    }

    std::string message( int ev ) const BOOST_OVERRIDE
    {
        return pc_->message( ev );
    }

    std::error_condition default_error_condition( int ev ) const noexcept BOOST_OVERRIDE
    {
        return pc_->default_error_condition( ev );
    }

    inline bool equivalent( int code, const std::error_condition & condition ) const noexcept BOOST_OVERRIDE;
    inline bool equivalent( const std::error_code & code, int condition ) const noexcept BOOST_OVERRIDE;
};

} // namespace detail

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_STD_CATEGORY_HPP_INCLUDED
