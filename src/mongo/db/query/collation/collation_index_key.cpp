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

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collation_index_key.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

// TODO SERVER-23172: Update this to consider strings inside nested objects or arrays.
bool CollationIndexKey::shouldUseCollationIndexKey(BSONElement elt,
                                                   const CollatorInterface* collator) {
    return collator && elt.type() == BSONType::String;
}

// TODO SERVER-23172: Update this to convert strings inside nested objects or arrays to their
// corresponding comparison keys.
void CollationIndexKey::collationAwareIndexKeyAppend(BSONElement elt,
                                                     const CollatorInterface* collator,
                                                     BSONObjBuilder* out) {
    if (shouldUseCollationIndexKey(elt, collator)) {
        auto comparisonKey = collator->getComparisonKey(elt.valueStringData());
        out->append("", comparisonKey.getKeyData());
    } else {
        out->appendAs(elt, "");
    }
}

}  // namespace mongo
