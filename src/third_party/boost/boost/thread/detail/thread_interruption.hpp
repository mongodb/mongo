#ifndef BOOST_THREAD_DETAIL_THREAD_INTERRUPTION_HPP
#define BOOST_THREAD_DETAIL_THREAD_INTERRUPTION_HPP
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// (C) Copyright 2007-9 Anthony Williams

namespace boost
{
    namespace this_thread
    {
        class BOOST_THREAD_DECL disable_interruption
        {
            disable_interruption(const disable_interruption&);
            disable_interruption& operator=(const disable_interruption&);
            
            bool interruption_was_enabled;
            friend class restore_interruption;
        public:
            disable_interruption();
            ~disable_interruption();
        };

        class BOOST_THREAD_DECL restore_interruption
        {
            restore_interruption(const restore_interruption&);
            restore_interruption& operator=(const restore_interruption&);
        public:
            explicit restore_interruption(disable_interruption& d);
            ~restore_interruption();
        };
    }
}

#endif
