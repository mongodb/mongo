/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace document_path_support {

/**
 * Calls 'callback' once for each value found at 'path' in the document 'doc'. If an array value is
 * found at 'path', it is expanded and 'callback' is invoked once for each value within the array.
 *
 * For example, 'callback' will be invoked on the values 1, 1, {a: 1}, 2 and 3 are on the path "x.y"
 * in the document {x: [{y: 1}, {y: 1}, {y: {a: 1}}, {y: [2, 3]}, 3, 4]}.
 */
void visitAllValuesAtPath(const Document& doc,
                          const FieldPath& path,
                          stdx::function<void(const Value&)> callback);

/**
 * Returns the element at 'path' in 'doc', or a missing Value if the path does not fully exist.
 *
 * Returns ErrorCodes::InternalError if an array is encountered along the path or at the end of the
 * path.
 */
StatusWith<Value> extractElementAlongNonArrayPath(const Document& doc, const FieldPath& path);

/**
 * Extracts 'paths' from the input document and returns a BSON object containing only those paths.
 */
BSONObj documentToBsonWithPaths(const Document&, const std::set<std::string>& paths);

}  // namespace document_path_support
}  // namespace mongo
