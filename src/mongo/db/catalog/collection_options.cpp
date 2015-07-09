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

#include "mongo/db/catalog/collection_options.h"

#include "mongo/base/string_data.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// static
bool CollectionOptions::validMaxCappedDocs(long long* max) {
    if (*max <= 0 || *max == std::numeric_limits<long long>::max()) {
        *max = 0x7fffffff;
        return true;
    }

    if (*max < (0x1LL << 31)) {
        return true;
    }

    return false;
}

void CollectionOptions::reset() {
    capped = false;
    cappedSize = 0;
    cappedMaxDocs = 0;
    initialNumExtents = 0;
    initialExtentSizes.clear();
    autoIndexId = DEFAULT;
    // For compatibility with previous versions if the user sets no flags,
    // we set Flag_UsePowerOf2Sizes in case the user downgrades.
    flags = Flag_UsePowerOf2Sizes;
    flagsSet = false;
    temp = false;
    storageEngine = BSONObj();
    validator = BSONObj();
    validationLevel = "";
    validationState = "";
}

bool CollectionOptions::isValid() const {
    return validate().isOK();
}

Status CollectionOptions::validate() const {
    return CollectionOptions().parse(toBSON());
}

Status CollectionOptions::parse(const BSONObj& options) {
    reset();

    // During parsing, ignore some validation errors in order to accept options objects that
    // were valid in previous versions of the server.  SERVER-13737.
    BSONObjIterator i(options);
    while (i.more()) {
        BSONElement e = i.next();
        StringData fieldName = e.fieldName();

        if (fieldName == "capped") {
            capped = e.trueValue();
        } else if (fieldName == "size") {
            if (!e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            cappedSize = e.numberLong();
            if (cappedSize < 0)
                return Status(ErrorCodes::BadValue, "size has to be >= 0");
            cappedSize += 0xff;
            cappedSize &= 0xffffffffffffff00LL;
        } else if (fieldName == "max") {
            if (!options["capped"].trueValue() || !e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            cappedMaxDocs = e.numberLong();
            if (!validMaxCappedDocs(&cappedMaxDocs))
                return Status(ErrorCodes::BadValue,
                              "max in a capped collection has to be < 2^31 or not set");
        } else if (fieldName == "$nExtents") {
            if (e.type() == Array) {
                BSONObjIterator j(e.Obj());
                while (j.more()) {
                    BSONElement inner = j.next();
                    initialExtentSizes.push_back(inner.numberInt());
                }
            } else {
                initialNumExtents = e.numberLong();
            }
        } else if (fieldName == "autoIndexId") {
            if (e.trueValue())
                autoIndexId = YES;
            else
                autoIndexId = NO;
        } else if (fieldName == "flags") {
            flags = e.numberInt();
            flagsSet = true;
        } else if (fieldName == "temp") {
            temp = e.trueValue();
        } else if (fieldName == "storageEngine") {
            // Storage engine-specific collection options.
            // "storageEngine" field must be of type "document".
            // Every field inside "storageEngine" has to be a document.
            // Format:
            // {
            //     ...
            //     storageEngine: {
            //         storageEngine1: {
            //             ...
            //         },
            //         storageEngine2: {
            //             ...
            //         }
            //     },
            //     ...
            // }
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'storageEngine' has to be a document.");
            }

            BSONForEach(storageEngineElement, e.Obj()) {
                StringData storageEngineName = storageEngineElement.fieldNameStringData();
                if (storageEngineElement.type() != mongo::Object) {
                    return Status(ErrorCodes::BadValue,
                                  str::stream() << "'storageEngine." << storageEngineName
                                                << "' has to be an embedded document.");
                }
            }

            storageEngine = e.Obj().getOwned();
        } else if (fieldName == "validator") {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'validator' has to be a document.");
            }

            validator = e.Obj().getOwned();
        } else if (fieldName == "validationState") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationState' has to be a string.");
            }

            validationState = e.String();
        } else if (fieldName == "validationLevel") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationLevel' has to be a string.");
            }

            validationLevel = e.String();
        }
    }

    return Status::OK();
}

BSONObj CollectionOptions::toBSON() const {
    BSONObjBuilder b;
    if (capped) {
        b.appendBool("capped", true);
        b.appendNumber("size", cappedSize);

        if (cappedMaxDocs)
            b.appendNumber("max", cappedMaxDocs);
    }

    if (initialNumExtents)
        b.appendNumber("$nExtents", initialNumExtents);
    if (!initialExtentSizes.empty())
        b.append("$nExtents", initialExtentSizes);

    if (autoIndexId != DEFAULT)
        b.appendBool("autoIndexId", autoIndexId == YES);

    if (flagsSet)
        b.append("flags", flags);

    if (temp)
        b.appendBool("temp", true);

    if (!storageEngine.isEmpty()) {
        b.append("storageEngine", storageEngine);
    }

    if (!validator.isEmpty()) {
        b.append("validator", validator);
    }

    if (!validationLevel.empty()) {
        b.append("validationLevel", validationLevel);
    }

    if (!validationState.empty()) {
        b.append("validationState", validationState);
    }

    return b.obj();
}
}
