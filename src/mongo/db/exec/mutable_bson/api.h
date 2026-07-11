// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/visibility.h"
#include "mongo/util/modules.h"

#ifdef MONGO_API_mutable_bson  // Compiling mutable_bson API
#define MONGO_MUTABLE_BSON_API MONGO_API_EXPORT
#else
#define MONGO_MUTABLE_BSON_API MONGO_API_IMPORT
#endif
