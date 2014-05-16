/*
 *    Copyright 2010 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#undef MONGO_EXPOSE_MACROS

// pragma push_macro only works in gcc 4.3+
// However, you had to define a special macro
// and build gcc yourself for it to work in 4.3.
// Version 4.4+ activate the feature by default.

#define GCC_VERSION (__GNUC__ * 10000                 \
                     + __GNUC_MINOR__ * 100           \
                     + __GNUC_PATCHLEVEL__)

#if GCC_VERSION >= 40402

# define malloc 42

# include "mongo/client/redef_macros.h"
# include "mongo/client/undef_macros.h"

# if malloc == 42
# else
#  error malloc macro molested
# endif

# undef malloc

#ifndef MONGO_MALLOC
#define MONGO_MALLOC 1
#endif

# define malloc 42

# include "mongo/client/redef_macros.h"
# include "mongo/client/undef_macros.h"

# if malloc == 42
# else
#  error malloc macro molested
# endif

# undef malloc


#endif // gcc 4.3

