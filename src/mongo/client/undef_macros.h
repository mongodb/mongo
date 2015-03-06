/** @file undef_macros.h remove mongo implementation macros after using */

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

// If you define a new global un-prefixed macro, please add it here and in redef_macros

// #pragma once // this file is intended to be processed multiple times

#ifdef MONGO_MACROS_PUSHED

// util/assert_util.h
#undef dassert
#pragma pop_macro("dassert")
#undef wassert
#pragma pop_macro("wassert")
#undef massert
#pragma pop_macro("massert")
#undef uassert
#pragma pop_macro("uassert")
#undef verify
#pragma pop_macro("verify")
#undef invariant
#pragma pop_macro("invariant")
#undef invariantOK
#pragma pop_macro("invariantOK")
#undef DESTRUCTOR_GUARD
#pragma pop_macro("DESTRUCTOR_GUARD")

// util/print.h
#undef PRINT
#pragma pop_macro("PRINT")
#undef PRINTFL
#pragma pop_macro("PRINTFL")

// util/debug_util.h
#undef DEV
#pragma pop_macro("DEV")
#undef SOMETIMES
#pragma pop_macro("SOMETIMES")
#undef OCCASIONALLY
#pragma pop_macro("OCCASIONALLY")
#undef RARELY
#pragma pop_macro("RARELY")
#undef ONCE
#pragma pop_macro("ONCE")

// util/log.h
#undef LOG
#pragma pop_macro("LOG")

#undef MONGO_MACROS_PUSHED
#endif
