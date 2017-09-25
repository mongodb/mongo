/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
constexpr StringData InternalSchemaXorMatchExpression::kName;

bool InternalSchemaXorMatchExpression::matches(const MatchableDocument* doc,
                                               MatchDetails* details) const {
    bool found = false;
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matches(doc, nullptr)) {
            if (found) {
                return false;
            }
            found = true;
        }
    }
    return found;
}

bool InternalSchemaXorMatchExpression::matchesSingleElement(const BSONElement& element,
                                                            MatchDetails* details) const {
    bool found = false;
    for (size_t i = 0; i < numChildren(); i++) {
        if (getChild(i)->matchesSingleElement(element, details)) {
            if (found) {
                return false;
            }
            found = true;
        }
    }
    return found;
}

void InternalSchemaXorMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << kName + "\n";
    _debugList(debug, level);
}

void InternalSchemaXorMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONArrayBuilder arrBob(out->subarrayStart(kName));
    _listToBSON(&arrBob);
}
}  //  namespace mongo
