/** @file bson.h

    Main bson include file for mongodb c++ clients. MongoDB includes ../db/jsobj.h instead.
    This file, however, pulls in much less code / dependencies.

    @see bsondemo
*/

/*
 *    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
   Main include file for C++ BSON. This pulls in fewer dependencies than
   mongo/client/dbclient.h, but still requires libmongoclient to link.

   "BSON" stands for "binary JSON" -- ie a binary way to represent objects that would be
   represented in JSON (plus a few extensions useful for databases & other languages).

   http://www.bsonspec.org/
*/

#pragma once

#ifdef MONGO_EXPOSE_MACROS
#error bson.h is for C++ driver consumer use only
#endif

#define LIBMONGOCLIENT_CONSUMER

#include "mongo/client/redef_macros.h"

#include "mongo/pch.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonobjiterator.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bson-inl.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"

#include "mongo/client/undef_macros.h"
