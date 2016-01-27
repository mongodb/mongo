/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_key_validate.h"

#include <cmath>
#include <limits>

#include "mongo/db/field_ref.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

Status validateKeyPattern(const BSONObj& key) {
    const ErrorCodes::Error code = ErrorCodes::CannotCreateIndex;

    if (key.objsize() > 2048)
        return Status(code, "Index key pattern too large.");

    if (key.isEmpty())
        return Status(code, "Index keys cannot be empty.");

    string pluginName = IndexNames::findPluginName(key);
    if (pluginName.size()) {
        if (!IndexNames::isKnownName(pluginName))
            return Status(
                code, mongoutils::str::stream() << "Unknown index plugin '" << pluginName << '\'');
    }

    BSONObjIterator it(key);
    while (it.more()) {
        BSONElement keyElement = it.next();

        if (keyElement.isNumber()) {
            double value = keyElement.number();
            if (std::isnan(value)) {
                return {code, "Values in the index key pattern cannot be NaN."};
            } else if (value == 0.0) {
                return {code, "Values in the index key pattern cannot be 0."};
            }
        } else if (keyElement.type() != String) {
            return {code,
                    str::stream() << "Values in index key pattern cannot be of type "
                                  << typeName(keyElement.type())
                                  << ". Only numbers > 0, numbers < 0, and strings are allowed."};
        }

        if (keyElement.type() == String && pluginName != keyElement.str()) {
            return Status(code, "Can't use more than one index plugin for a single index.");
        }

        // Ensure that the fields on which we are building the index are valid: a field must not
        // begin with a '$' unless it is part of a DBRef or text index, and a field path cannot
        // contain an empty field. If a field cannot be created or updated, it should not be
        // indexable.

        FieldRef keyField(keyElement.fieldName());

        const size_t numParts = keyField.numParts();
        if (numParts == 0) {
            return Status(code, "Index keys cannot be an empty field.");
        }

        // "$**" is acceptable for a text index.
        if (mongoutils::str::equals(keyElement.fieldName(), "$**") &&
            keyElement.valuestrsafe() == IndexNames::TEXT)
            continue;

        if (mongoutils::str::equals(keyElement.fieldName(), "_fts") &&
            keyElement.valuestrsafe() != IndexNames::TEXT) {
            return Status(code, "Index key contains an illegal field name: '_fts'");
        }

        for (size_t i = 0; i != numParts; ++i) {
            const StringData part = keyField.getPart(i);

            // Check if the index key path contains an empty field.
            if (part.empty()) {
                return Status(code, "Index keys cannot contain an empty field.");
            }

            if (part[0] != '$')
                continue;

            // Check if the '$'-prefixed field is part of a DBRef: since we don't have the
            // necessary context to validate whether this is a proper DBRef, we allow index
            // creation on '$'-prefixed names that match those used in a DBRef.
            const bool mightBePartOfDbRef =
                (i != 0) && (part == "$db" || part == "$id" || part == "$ref");

            if (!mightBePartOfDbRef) {
                return Status(code,
                              "Index key contains an illegal field name: "
                              "field name starts with '$'.");
            }
        }
    }

    return Status::OK();
}
}  // namespace mongo
