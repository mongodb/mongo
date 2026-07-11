// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/json_pointer.h"

#include "mongo/util/assert_util.h"

#include <cstddef>
#include <utility>

namespace {


//  Check for unespaced '~' characters, then replace all occurrences of '~1' with '/', and '~0'
//  with '~'.
std::string replaceEscapeChars(std::string str) {
    size_t tildaLoc = 0;
    // Search the string for any '~', and replace it if it is part of '~1'.
    while ((tildaLoc = str.find('~', tildaLoc)) != std::string::npos) {
        uassert(51063,
                "JSONPointer cannot contain unescaped ~ character",
                str.length() > tildaLoc + 1 &&
                    (str[tildaLoc + 1] == '0' || str[tildaLoc + 1] == '1'));
        // If we can replace the '~' in this loop, do it now.
        if (str[tildaLoc + 1] == '1') {
            str.replace(tildaLoc, 2, "/");
        }
        ++tildaLoc;
    }

    // Replace all '~0' with '~' after all other tilda characters have been removed.
    size_t escapeLoc = 0;
    while ((escapeLoc = str.find("~0", escapeLoc)) != std::string::npos) {
        // Replace with '~' and increment to ensure that '~' isn't checked twice.
        str.replace(escapeLoc++, 2, "~");
    }
    return str;
}
}  // namespace

namespace mongo {

JSONPointer::JSONPointer(std::string ptr) {
    // Check if pointer specifies root.
    uassert(51064, "Empty JSONPointers are not supported", ptr.length() != 0);
    uassert(51065, "JSONPointer must start with a '/'", ptr[0] == '/');
    size_t startOfKeyIndex = 1;
    size_t nextSlashIndex = 0;
    std::string key;
    while ((nextSlashIndex = ptr.find('/', startOfKeyIndex)) != std::string::npos) {
        key = ptr.substr(startOfKeyIndex, nextSlashIndex - startOfKeyIndex);
        key = replaceEscapeChars(std::move(key));
        _parsed.push_back(std::move(key));
        startOfKeyIndex = nextSlashIndex + 1;
    }
    // Parse the last key.
    nextSlashIndex = ptr.size();
    key = ptr.substr(startOfKeyIndex, nextSlashIndex - startOfKeyIndex);
    key = replaceEscapeChars(std::move(key));
    _parsed.push_back(std::move(key));

    _original = ptr;
}

BSONElement JSONPointer::evaluate(const BSONObj& obj) const {
    auto curObj = obj;
    int numKeys = _parsed.size();
    for (int i = 0; i < numKeys; ++i) {
        auto key = _parsed[i];
        auto nextElem = curObj[key];
        if (!nextElem) {
            break;
        }

        // if this is the last key, return the found element.
        if (i == numKeys - 1) {
            return nextElem;
        }
        // If this is not the last key, and the current element is not an object with
        // keys, pointer does not match.
        if (!nextElem.isABSONObj()) {
            break;
        }
        // This relies on the behavior that a BSONArray is formatted as a BSONObj with numeric,
        // ascending keys.
        curObj = nextElem.embeddedObject();
    }

    return BSONElement();
}

}  // namespace mongo
