// stdafx.cpp : source file that includes just the standard includes

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

#include "stdafx.h"

#if defined( __MSVC__ )
// should probably check VS version here
#elif defined( __GNUC__ )

#if __GNUC__ < 4
#error gcc < 4 not supported
#endif

#else
// unknown compiler
#endif 


namespace mongo {

    const char versionString[] = "1.1.1-";

} // namespace mongo
