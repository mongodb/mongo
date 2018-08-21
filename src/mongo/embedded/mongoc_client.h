/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#ifndef HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
#define HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED

// <inttypes.h> is needed to avoid macro redefinition error when compiling on Windows.
// Should be fixed inside mongoc.h
#include <inttypes.h>
#include <mongo/embedded/capi.h>
#include <mongoc.h>

#pragma push_macro("MONGO_API_CALL")
#undef MONGO_API_CALL

#pragma push_macro("MONGO_API_IMPORT")
#undef MONGO_API_IMPORT

#pragma push_macro("MONGO_API_EXPORT")
#undef MONGO_API_EXPORT

#pragma push_macro("MONGO_EMBEDDED_MONGOC_CLIENT_API")
#undef MONGO_EMBEDDED_MONGOC_CLIENT_API

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#define MONGO_API_IMPORT __declspec(dllimport)
#define MONGO_API_EXPORT __declspec(dllexport)
#else
#define MONGO_API_CALL
#define MONGO_API_IMPORT __attribute__((visibility("default")))
#define MONGO_API_EXPORT __attribute__((used, visibility("default")))
#endif

#if defined(MONGO_EMBEDDED_MONGOC_CLIENT_STATIC)
#define MONGO_EMBEDDED_MONGOC_CLIENT_API
#else
#if defined(MONGO_EMBEDDED_MONGOC_CLIENT_COMPILING)
#define MONGO_EMBEDDED_MONGOC_CLIENT_API MONGO_API_EXPORT
#else
#define MONGO_EMBEDDED_MONGOC_CLIENT_API MONGO_API_IMPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a client with the correct stream intiator set
 * @param db must be a valid instance handle created by `mongo_embedded_v1_instance_create`
 * @returns a mongoc client or `NULL` on error
 */
MONGO_EMBEDDED_MONGOC_CLIENT_API mongoc_client_t* MONGO_API_CALL
mongo_embedded_v1_mongoc_client_create(mongo_embedded_v1_instance* instance);

#ifdef __cplusplus
}  // extern "C"
#endif

#undef MONGO_EMBEDDED_MONGOC_CLIENT_API
#pragma pop_macro("MONGO_EMBEDDED_MONGOC_CLIENT_API")

#undef MONGO_API_EXPORT
#pragma push_macro("MONGO_API_EXPORT")

#undef MONGO_API_IMPORT
#pragma push_macro("MONGO_API_IMPORT")

#undef MONGO_API_CALL
#pragma pop_macro("MONGO_API_CALL")

#endif  // HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
