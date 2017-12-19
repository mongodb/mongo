/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * Confirms that obj only contains field names where allowed(name) returns true,
 * and that no field name occurs multiple times.
 *
 * On failure, returns BadValue and a message naming the unexpected field or DuplicateKey and a
 * message naming the repeated field.  "objectName" is included in the message, for reporting
 * purposes.
 */
template <typename Condition>
Status bsonCheckOnlyHasFieldsImpl(StringData objectName,
                                  const BSONObj& obj,
                                  const Condition& allowed) {
    StringMap<bool> seenFields;
    for (auto&& e : obj) {
        const auto name = e.fieldNameStringData();

        if (!allowed(name)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Unexpected field " << e.fieldName() << " in "
                                        << objectName);
        }

        bool& seenBefore = seenFields[name];
        if (!seenBefore) {
            seenBefore = true;
        } else {
            return Status(ErrorCodes::DuplicateKey,
                          str::stream() << "Field " << name << " appears multiple times in "
                                        << objectName);
        }
    }
    return Status::OK();
}

/**
 * Like above but only allows fields from the passed-in container.
 */
template <typename Container>
Status bsonCheckOnlyHasFields(StringData objectName,
                              const BSONObj& obj,
                              const Container& allowedFields) {
    return bsonCheckOnlyHasFieldsImpl(objectName, obj, [&](StringData name) {
        return std::find(std::begin(allowedFields), std::end(allowedFields), name) !=
            std::end(allowedFields);
    });
}

/**
 * Like above but only allows fields from the passed-in container or are generic command arguments.
 */
template <typename Container>
Status bsonCheckOnlyHasFieldsForCommand(StringData objectName,
                                        const BSONObj& obj,
                                        const Container& allowedFields) {
    return bsonCheckOnlyHasFieldsImpl(objectName, obj, [&](StringData name) {
        return Command::isGenericArgument(name) ||
            (std::find(std::begin(allowedFields), std::end(allowedFields), name) !=
             std::end(allowedFields));
    });
}

/**
 * Throws a uassert if the type of the elem does not match that provided in expectedType
 */
inline void checkBSONType(BSONType expectedType, const BSONElement& elem) {
    uassert(elem.type() == BSONType::EOO ? ErrorCodes::NoSuchKey : ErrorCodes::TypeMismatch,
            str::stream() << "Wrong type for '" << elem.fieldNameStringData() << "'. Expected a "
                          << typeName(expectedType)
                          << ", got a "
                          << typeName(elem.type())
                          << '.',
            elem.type() == expectedType);
}


}  // namespace mongo
