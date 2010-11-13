// @file dur_journal.cpp

/**
*    Copyright (C) 2010 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#if defined(_DURABLE)

#include "dur_journal.h"
#include "../util/logfile.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert MONGO_assert


namespace mongo {
    namespace dur {
        BOOST_STATIC_ASSERT( sizeof(JSectHeader) == 8 );
        BOOST_STATIC_ASSERT( sizeof(JSectFooter) == 8 );
        BOOST_STATIC_ASSERT( sizeof(JEntry) == 6 );

    }
}

#endif
