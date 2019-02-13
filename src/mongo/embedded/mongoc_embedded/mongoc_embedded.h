/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#ifndef HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
#define HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED

#include <mongo_embedded/mongo_embedded.h>
#include <mongoc/mongoc.h>

#pragma push_macro("MONGO_API_CALL")
#undef MONGO_API_CALL

#pragma push_macro("MONGO_API_IMPORT")
#undef MONGO_API_IMPORT

#pragma push_macro("MONGO_API_EXPORT")
#undef MONGO_API_EXPORT

#pragma push_macro("MONGOC_EMBEDDED_API")
#undef MONGOC_EMBEDDED_API

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#define MONGO_API_IMPORT __declspec(dllimport)
#define MONGO_API_EXPORT __declspec(dllexport)
#else
#define MONGO_API_CALL
#define MONGO_API_IMPORT __attribute__((visibility("default")))
#define MONGO_API_EXPORT __attribute__((used, visibility("default")))
#endif

#if defined(MONGOC_EMBEDDED_STATIC)
#define MONGOC_EMBEDDED_API
#else
#if defined(MONGOC_EMBEDDED_COMPILING)
#define MONGOC_EMBEDDED_API MONGO_API_EXPORT
#else
#define MONGOC_EMBEDDED_API MONGO_API_IMPORT
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
MONGOC_EMBEDDED_API mongoc_client_t* MONGO_API_CALL
mongoc_embedded_v1_client_create(mongo_embedded_v1_instance* instance);

#ifdef __cplusplus
}  // extern "C"
#endif

#undef MONGOC_EMBEDDED_API
#pragma pop_macro("MONGOC_EMBEDDED_API")

#undef MONGO_API_EXPORT
#pragma push_macro("MONGO_API_EXPORT")

#undef MONGO_API_IMPORT
#pragma push_macro("MONGO_API_IMPORT")

#undef MONGO_API_CALL
#pragma pop_macro("MONGO_API_CALL")

#endif  // HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
