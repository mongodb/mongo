// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/modules.h"

namespace mongo::exec::expression::serialize_ejson_utils {

/**
 * Transform a BSON value to the equivalent Extended JSON, represented in BSON.
 * The 'value' is mapped to Extended JSON following the Extended JSON v2 specification.
 * The 'value' cannot be missing/BSONType::eoo. The parameter 'relaxed' selects between the Relaxed
 * and Canonical specification.
 * Note: In Relaxed mode, the types of numeric values are preserved in the result.
 * This transformation can be reversed by deserializeFromExtendedJson.
 */
Value serializeToExtendedJson(const Value& value, bool relaxed);

/**
 * Transform an Extended JSON-compatible description of a BSON value back into BSON.
 * The input 'value' can represent either Relaxed Extended JSON or Canonical Extended JSON.
 * The 'value' cannot be missing/BSONType::eoo. This is the reverse of serializeToExtendedJson.
 */
Value deserializeFromExtendedJson(const Value& value);

}  // namespace mongo::exec::expression::serialize_ejson_utils
