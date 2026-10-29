// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/optime.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
BSONObj OplogEntryOrGroupedInserts::toBSON() const {
    if (!isGroupedInserts())
        return getOp()->getEntry().toBSON();

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
        for (const auto& op : _entryOrGroupedInserts) {
            tsArrayBuilder.append(op->getTimestamp());
        }
    }
    // Populate the "t" (term) field with an array of all the grouped inserts' terms.
    {
        BSONArrayBuilder tArrayBuilder(groupedInsertBuilder.subarrayStart("t"));
        for (const auto& op : _entryOrGroupedInserts) {
            long long term = OpTime::kUninitializedTerm;
            auto parsedTerm = op->getTerm();
            if (parsedTerm)
                term = parsedTerm.value();
            tArrayBuilder.append(term);
        }
    }
    // Populate the "o" field with an array of all the grouped inserts.
    {
        BSONArrayBuilder oArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
        for (const auto& op : _entryOrGroupedInserts) {
            oArrayBuilder.append(op->getObject());
        }
    }
    // Generate an op object of all elements except for "ts", "t", and "o", since we
    // need to make those fields arrays of all the ts's, t's, and o's.
    groupedInsertBuilder.appendElementsUnique(getOp()->getEntry().toBSON());

    return groupedInsertBuilder.obj();
}
}  // namespace repl
}  // namespace mongo
