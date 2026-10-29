// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/util/modules.h"

// Forward declarations of our mongocrypt dependencies. Required because we don't want
// mongocrypt headers transitively included by other targets, polluting the global namespace.
extern "C" {
struct _mc_FLE2IndexedEncryptedValueV2_t;
struct _mc_FLE2TagAndEncryptedMetadataBlock_t;
struct _mongocrypt_t;
void mc_FLE2IndexedEncryptedValueV2_destroy(struct _mc_FLE2IndexedEncryptedValueV2_t*);
}  // extern "C"
