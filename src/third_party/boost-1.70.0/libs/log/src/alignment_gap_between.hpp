/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   alignment_gap_between.hpp
 * \author Andrey Semashev
 * \date   20.11.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_ALIGNMENT_GAP_BETWEEN_HPP_INCLUDED_
#define BOOST_LOG_ALIGNMENT_GAP_BETWEEN_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/type_traits/alignment_of.hpp>
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

//! The metafunction computes the minimal gap between objects t1 and t2 of types T1 and T2
//! that would be needed to maintain the alignment of t2 if it's placed right after t1
template< typename T1, typename T2 >
struct alignment_gap_between
{
    enum _
    {
        T2_alignment = boost::alignment_of< T2 >::value,
        tail_size = sizeof(T1) % T2_alignment,
        value = tail_size > 0 ? T2_alignment - tail_size : 0
    };
};

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_ALIGNMENT_GAP_BETWEEN_HPP_INCLUDED_
