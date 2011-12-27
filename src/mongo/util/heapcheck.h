// @file heapcheck.h

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

#pragma once

#if defined(HEAP_CHECKING)

#include <google/heap-checker.h>

#define IGNORE_OBJECT( a ) HeapLeakChecker::IgnoreObject( a )
#define UNIGNORE_OBJECT( a ) HeapLeakChecker::UnIgnoreObject( a )

#else

#define IGNORE_OBJECT( a )
#define UNIGNORE_OBJECT( a )

#endif
