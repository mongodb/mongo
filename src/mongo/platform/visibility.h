// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#if !defined(MONGO_USE_VISIBILITY)

#define MONGO_API_EXPORT
#define MONGO_API_IMPORT
#define MONGO_PRIVATE

#else

#if defined(_MSC_VER)

#define MONGO_API_EXPORT __declspec(dllexport)
#define MONGO_API_IMPORT __declspec(dllimport)
#define MONGO_PRIVATE

#else

#define MONGO_API_EXPORT __attribute__((visibility("default")))
#define MONGO_API_IMPORT __attribute__((visibility("default")))
#define MONGO_PRIVATE __attribute__((visibility("hidden")))

#endif

#endif
