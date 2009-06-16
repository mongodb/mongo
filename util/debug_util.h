// debug_util.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

namespace mongo {

// for debugging
    typedef struct _Ints {
        int i[100];
    } *Ints;
    typedef struct _Chars {
        char c[200];
    } *Chars;

    typedef char CHARS[400];

    typedef struct _OWS {
        int size;
        char type;
        char string[400];
    } *OWS;

// for now, running on win32 means development not production --
// use this to log things just there.
#if defined(_WIN32)
#define WIN if( 1 )
#else
#define WIN if( 0 )
#endif

#if defined(_DEBUG)
#define DEV if( 1 )
#else
#define DEV if( 0 )
#endif

#define DEBUGGING if( 0 )

// The following declare one unique counter per enclosing function.
// NOTE The implementation double-increments on a match, but we don't really care.
#define SOMETIMES( occasion, howOften ) for( static unsigned occasion = 0; ++occasion % howOften == 0; )
#define OCCASIONALLY SOMETIMES( occasionally, 16 )
#define RARELY SOMETIMES( rarely, 128 )
#define ONCE for( static bool undone = true; undone; undone = false ) 
    
#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

} // namespace mongo
