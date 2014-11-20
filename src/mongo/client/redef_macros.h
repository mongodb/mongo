/** @file redef_macros.h macros for mongo internals
    
    @see undef_macros.h undefines these after use to minimize name pollution.
*/

/*    Copyright 2009 10gen Inc.
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

// If you define a new global un-prefixed macro, please add it here and in undef_macros

#define MONGO_MACROS_PUSHED 1

// util/assert_util.h
#pragma push_macro("verify")
#undef verify
#define verify MONGO_verify
#pragma push_macro("invariant")
#undef invariant
#define invariant MONGO_invariant
#pragma push_macro("invariantOK")
#undef invariantOK
#define invariantOK MONGO_invariantOK
#pragma push_macro("dassert")
#undef dassert
#define dassert MONGO_dassert
#pragma push_macro("wassert")
#undef wassert
#define wassert MONGO_wassert
#pragma push_macro("massert")
#undef massert
#define massert MONGO_massert
#pragma push_macro("uassert")
#undef uassert
#define uassert MONGO_uassert
#pragma push_macro("DESTRUCTOR_GUARD")
#undef DESTRUCTOR_GUARD
#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD

// util/goodies.h
#pragma push_macro("PRINT")
#undef PRINT
#define PRINT MONGO_PRINT
#pragma push_macro("PRINTFL")
#undef PRINTFL
#define PRINTFL MONGO_PRINTFL

// util/debug_util.h
#pragma push_macro("DEV")
#undef DEV
#define DEV MONGO_DEV
#pragma push_macro("DEBUGGING")
#undef DEBUGGING
#define DEBUGGING MONGO_DEBUGGING
#pragma push_macro("SOMETIMES")
#undef SOMETIMES
#define SOMETIMES MONGO_SOMETIMES
#pragma push_macro("OCCASIONALLY")
#undef OCCASIONALLY
#define OCCASIONALLY MONGO_OCCASIONALLY
#pragma push_macro("RARELY")
#undef RARELY
#define RARELY MONGO_RARELY
#pragma push_macro("ONCE")
#undef ONCE
#define ONCE MONGO_ONCE

// util/log.h
#pragma push_macro("LOG")
#undef LOG
#define LOG MONGO_LOG


