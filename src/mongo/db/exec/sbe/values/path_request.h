// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/modules.h"

#include <iosfwd>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

    std::string_view getTopLevelField() const;

    std::string getFullPath() const;
};

std::string pathToString(const Path& p);
std::ostream& operator<<(std::ostream& os, const Path& path);
}  // namespace mongo::sbe::value
