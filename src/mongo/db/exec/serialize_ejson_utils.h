/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
