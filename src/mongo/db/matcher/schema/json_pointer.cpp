/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/json_pointer.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

namespace {


//  Check for unespaced '~' characters, then replace all occurrences of '~1' with '/', and '~0'
//  with '~'.
std::string replaceEscapeChars(std::string str) {
    size_t tildaLoc = 0;
    // Search the string for any '~', and replace it if it is part of '~1'.
    while ((tildaLoc = str.find("~", tildaLoc)) != std::string::npos) {
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
        str.replace(escapeLoc, 2, "~");
    }
    return str;
}
}  // namespace

namespace mongo {

JSONPointer::JSONPointer(const std::string& ptr) {
    // Check if pointer specifies root.
    uassert(51064, "Empty JSONPointers are not supported", ptr.length() != 0);
    uassert(51065, "JSONPointer must start with a '/'", ptr[0] == '/');
    size_t startOfKeyIndex = 1;
    size_t nextSlashIndex = 0;
    std::string key;
    while ((nextSlashIndex = ptr.find("/", startOfKeyIndex)) != std::string::npos) {
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
