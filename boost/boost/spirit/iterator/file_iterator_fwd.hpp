/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(BOOST_SPIRIT_FILE_ITERATOR_FWD_HPP)
#define BOOST_SPIRIT_FILE_ITERATOR_FWD_HPP

namespace boost { namespace spirit {

    namespace fileiter_impl 
    {
        template <typename CharT = char>
        class std_file_iterator;

        // may never be defined -- so what...
        template <typename CharT = char>
        class mmap_file_iterator;
    } 

    // no defaults here -- too much dependencies
    template <
        typename CharT,
        typename BaseIterator
    > class file_iterator;

}} // namespace boost::spirit

#endif

