/*    Copyright 2013 10gen Inc.
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

#pragma once

#include "mongo/platform/compiler.h"

/**
 * Definition of macros used to label the mongo client api.
 *
 * If a type, free function or global variable is part of the client api, it must be labeled.
 *
 * To label a type, place the MONGO_CLIENT_API macro after the struct, class or enum keyword.
 * Example:
 *   class MONGO_CLIENT_API DBClientInterface { ... };
 *
 * To label a function, place the label on the declaration before the return type.  You
 * do NOT need to label the methods of exported classes.
 * Example:
 *   MONGO_CLIENT_API Status myFreeFunction(int arg1);
 *
 * To label a global variable, place the label on the declaration, before the type and
 * after the "extern" keyword.
 * Example:
 *   extern MONGO_CLIENT_API int myGlobalVariable;
 *
 * dbclient.h sets the LIBMONGOCLIENT_CONSUMER macro, so all clients will convert the
 * MONGO_CLIENT_API macro to the the import form, while the library code will convert it to the
 * export form.
 */

#if defined(LIBMONGOCLIENT_CONSUMER) && !defined(LIBMONGOCLIENT_BUILDING)
#define MONGO_CLIENT_API MONGO_COMPILER_API_IMPORT
#elif !defined(LIBMONGOCLIENT_CONSUMER) && defined(LIBMONGOCLIENT_BUILDING)
#define MONGO_CLIENT_API MONGO_COMPILER_API_EXPORT
#elif !defined(LIBMONGOCLIENT_CONSUMER) && !defined(LIBMONGOCLIENT_BUILDING)
#define MONGO_CLIENT_API
#else
#error "Must not define both LIBMONGOCLIENT_BUILDING and LIBMONGOCLIENT_CONSUMER"
#endif
