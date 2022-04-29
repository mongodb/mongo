/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/index/column_key_generator.h"

namespace mongo {
namespace column_keygen {
/**
 * Write 'element' to the end of 'cellBuffer' using a specialized compact format for values stored
 * in a columnar index "cell."
 */
void appendElementToCell(const BSONElement& element, BufBuilder* cellBuffer);

/**
 * Transform the contents of a columnar index "cell" into its on-disk storage format and write it
 * out to the 'cellBuffer' builder. The resulting cell encodes all the values associated with one
 * path in an indexed document as well as the associated "array info" and flags.
 */
void writeEncodedCell(const UnencodedCellView& cell, BufBuilder* cellBuffer);
}  // namespace column_keygen
}  // namespace mongo
