/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>

namespace mongo {

/**
 * A CollationSpec is a parsed representation of a user-provided collation BSONObj. Can be
 * re-serialized to BSON using the CollationSpecSerializer.
 *
 * TODO SERVER-22373: extend to support options other than the localeID.
 */
struct CollationSpec {
    // Field name constants.
    static const char* kLocaleField;

    // A string such as "en_US", identifying the language, country, or other attributes of the
    // locale for this collation.
    std::string localeID;
};

/**
 * Returns whether 'left' and 'right' are logically equivalent collations.
 */
inline bool operator==(const CollationSpec& left, const CollationSpec& right) {
    return left.localeID == right.localeID;
}

inline bool operator!=(const CollationSpec& left, const CollationSpec& right) {
    return !(left == right);
}

}  // namespace mongo
