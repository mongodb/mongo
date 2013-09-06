/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/ops/field_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

namespace fieldchecker {

    namespace {

        Status isUpdatable(const FieldRef& field, bool legacy) {
            const size_t numParts = field.numParts();

            if (numParts == 0) {
                return Status(ErrorCodes::BadValue, "cannot update an empty field name");
            }

            for (size_t i = 0; i != numParts; ++i) {
                const StringData part = field.getPart(i);

                if ((i == 0) && part.compare("_id") == 0) {
                    return Status(ErrorCodes::BadValue,
                                  "update cannot affect the _id");
                }

                if (part.empty()) {
                    return Status(ErrorCodes::BadValue,
                                  mongoutils::str::stream() << field.dottedField()
                                  << " contains empty fields");
                }

                if (!legacy && (part[0] == '$')) {

                    // A 'bare' dollar sign not in the first position is a positional
                    // update token, so it is not an error.
                    //
                    // TODO: In 'isPositional' below, we redo a very similar walk and check.
                    // Perhaps we should fuse these operations, and have isUpdatable take a
                    // 'PositionalContext' object to be populated with information about any
                    // discovered positional ops.
                    const bool positional = ((i != 0) && (part.size() == 1));

                    if (!positional) {

                        // We ignore the '$'-prefixed names that are part of a DBRef, because
                        // we don't have enough context here to validate that we have a proper
                        // DB ref. Errors with the DBRef will be caught upstream when
                        // okForStorage is invoked.
                        //
                        // TODO: We need to find a way to consolidate this checking with that
                        // done in okForStorage. There is too much duplication between this
                        // code and that code.
                        const bool mightBePartOfDbRef =
                            part.startsWith("$db") ||
                            part.startsWith("$id") ||
                            part.startsWith("$ref");

                        if (!mightBePartOfDbRef)
                            return Status(ErrorCodes::BadValue,
                                          mongoutils::str::stream() << field.dottedField()
                                          << " contains field names with leading '$' character");

                    }

                }

            }

            return Status::OK();
        }

    } // namespace

    Status isUpdatable(const FieldRef& field) {
        return isUpdatable(field, false);
    }

    Status isUpdatableLegacy(const FieldRef& field) {
        return isUpdatable(field, true);
    }

    bool isPositional(const FieldRef& fieldRef, size_t* pos, size_t* count) {

        // 'count' is optional.
        size_t dummy;
        if (count == NULL) {
            count = &dummy;
        }

        *count = 0;
        size_t size = fieldRef.numParts();
        for (size_t i=0; i<size; i++) {
            StringData fieldPart = fieldRef.getPart(i);
            if ((fieldPart.size() == 1) && (fieldPart[0] == '$')) {
                if (*count == 0) *pos = i;
                (*count)++;
            }
        }
        return *count > 0;
    }

} // namespace fieldchecker
} // namespace mongo
