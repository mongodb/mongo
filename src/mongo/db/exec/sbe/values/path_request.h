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

#include "mongo/base/string_data.h"
#include "mongo/util/modules.h"

#include <string>
#include <variant>

#pragma once
namespace mongo::sbe::value {

/**
 * Limited version of the path language supporting only Get, Traverse, and Id.  For now paths
 * consisting of these operations can be evaluated below the query layer.
 */

/**
 * Tries to get 'field' from the object and run the remainder of the path on the value at that
 * field.
 */
struct Get {
    std::string field;
};

/**
 * Indicates that an array should be traversed.
 * If the input IS an array, this applies the remainder of the path on every element.
 * If the input IS NOT an array this applies the remainder of the path to the input directly.
 */
struct Traverse {
    // Nothing
};

/**
 * Id component that returns its input (an identity function).
 */
struct Id {
    // Nothing.
};

using Component = std::variant<Get, Traverse, Id>;
using Path = std::vector<Component>;
enum PathRequestType { kFilter, kProject };

struct PathRequest {
    PathRequest() = delete;
    PathRequest(PathRequestType t) : type(t) {}
    PathRequest(PathRequestType t, Path p) : type(t), path(std::move(p)) {}

    PathRequestType type;

    // The path requested (ie which fields).
    Path path;

    // TODO: May want some other information here, like if we know we can omit certain values
    // etc etc or if we want to specify which type of position info will be needed.

    std::string toString() const;

    StringData getTopLevelField() const;

    std::string getFullPath() const;
};

std::string pathToString(const Path& p);
std::ostream& operator<<(std::ostream& os, const Path& path);
}  // namespace mongo::sbe::value
