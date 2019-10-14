/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace repl {
BSONObj OplogEntryOrGroupedInserts::toBSON() const {
    if (!isGroupedInserts())
        return getOp().toBSON();

    // Since we found more than one document, create grouped insert of many docs.
    // We are going to group many 'i' ops into one big 'i' op, with array fields for
    // 'ts', 't', and 'o', corresponding to each individual op.
    // For example:
    // { ts: Timestamp(1,1), t:1, ns: "test.foo", op:"i", o: {_id:1} }
    // { ts: Timestamp(1,2), t:1, ns: "test.foo", op:"i", o: {_id:2} }
    // become:
    // { ts: [Timestamp(1, 1), Timestamp(1, 2)],
    //    t: [1, 1],
    //    o: [{_id: 1}, {_id: 2}],
    //   ns: "test.foo",
    //   op: "i"
    // }
    // This BSONObj is used for error messages logging only.
    BSONObjBuilder groupedInsertBuilder;
    // Populate the "ts" field with an array of all the grouped inserts' timestamps.
    {
        BSONArrayBuilder tsArrayBuilder(groupedInsertBuilder.subarrayStart("ts"));
        for (auto op : _entryOrGroupedInserts) {
            tsArrayBuilder.append(op->getTimestamp());
        }
    }
    // Populate the "t" (term) field with an array of all the grouped inserts' terms.
    {
        BSONArrayBuilder tArrayBuilder(groupedInsertBuilder.subarrayStart("t"));
        for (auto op : _entryOrGroupedInserts) {
            long long term = OpTime::kUninitializedTerm;
            auto parsedTerm = op->getTerm();
            if (parsedTerm)
                term = parsedTerm.get();
            tArrayBuilder.append(term);
        }
    }
    // Populate the "o" field with an array of all the grouped inserts.
    {
        BSONArrayBuilder oArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
        for (auto op : _entryOrGroupedInserts) {
            oArrayBuilder.append(op->getObject());
        }
    }
    // Generate an op object of all elements except for "ts", "t", and "o", since we
    // need to make those fields arrays of all the ts's, t's, and o's.
    groupedInsertBuilder.appendElementsUnique(getOp().toBSON());
    return groupedInsertBuilder.obj();
}
}  // namespace repl
}  // namespace mongo
