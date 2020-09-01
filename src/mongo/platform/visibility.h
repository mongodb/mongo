/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#if !defined(MONGO_USE_VISIBILITY)

#define MONGO_VISIBILITY_PUBLIC()
#define MONGO_VISIBILITY_PRIVATE()
#define MONGO_PRIVATE
#define MONGO_API(LIB)

#else

#if defined(_MSC_VER)

#include <boost/preprocessor/control/iif.hpp>
#include <boost/vmd/is_number.hpp>

#define MONGO_DLLEXPORT() __declspec(dllexport)
#define MONGO_DLLIMPORT() __declspec(dllimport)
#define MONGO_PRIVATE
#define MONGO_API_IMPL2(COND) BOOST_PP_IIF(COND, MONGO_DLLEXPORT, MONGO_DLLIMPORT)()
#define MONGO_API_IMPL(ARG) MONGO_API_IMPL2(BOOST_VMD_IS_NUMBER(ARG))
#define MONGO_API(LIB) MONGO_API_IMPL(MONGO_API_##LIB)

#else

#define MONGO_VISIBILITY_PUBLIC() __attribute__((visibility("default")))
#define MONGO_VISIBILITY_PRIVATE() __attribute__((visibility("hidden")))
#define MONGO_PRIVATE MONGO_VISIBILITY_PRIVATE()
#define MONGO_API(LIB) MONGO_VISIBILITY_PUBLIC()

#endif

#endif
