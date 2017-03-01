/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

/**
 * A MockRange represents a range with endpoint that a MockNSTargeter uses to direct writes to
 * a particular endpoint.
 */
struct MockRange {
    MockRange(const ShardEndpoint& endpoint,
              const NamespaceString nss,
              const BSONObj& minKey,
              const BSONObj& maxKey)
        : endpoint(endpoint), range(nss.ns(), minKey, maxKey, getKeyPattern(minKey)) {}

    MockRange(const ShardEndpoint& endpoint, const KeyRange& range)
        : endpoint(endpoint), range(range) {}

    static BSONObj getKeyPattern(const BSONObj& key) {
        BSONObjIterator it(key);
        BSONObjBuilder objB;
        while (it.more())
            objB.append(it.next().fieldName(), 1);
        return objB.obj();
    }

    const ShardEndpoint endpoint;
    const KeyRange range;
};

/**
 * A MockNSTargeter directs writes to particular endpoints based on a list of MockRanges given
 * to the mock targeter on initialization.
 *
 * No refreshing behavior is currently supported.
 */
class MockNSTargeter : public NSTargeter {
public:
    void init(const std::vector<MockRange*> mockRanges) {
        ASSERT(!mockRanges.empty());
        _mockRanges.mutableVector().insert(
            _mockRanges.mutableVector().end(), mockRanges.begin(), mockRanges.end());
        _nss = NamespaceString(_mockRanges.vector().front()->range.ns);
    }

    const NamespaceString& getNS() const {
        return _nss;
    }

    /**
     * Returns a ShardEndpoint for the doc from the mock ranges
     */
    Status targetInsert(OperationContext* txn, const BSONObj& doc, ShardEndpoint** endpoint) const {
        std::vector<ShardEndpoint*> endpoints;
        Status status = targetQuery(doc, &endpoints);
        if (!status.isOK())
            return status;
        if (!endpoints.empty())
            *endpoint = endpoints.front();
        return Status::OK();
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }.
     */
    Status targetUpdate(OperationContext* txn,
                        const BatchedUpdateDocument& updateDoc,
                        std::vector<ShardEndpoint*>* endpoints) const {
        return targetQuery(updateDoc.getQuery(), endpoints);
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }.
     */
    Status targetDelete(OperationContext* txn,
                        const BatchedDeleteDocument& deleteDoc,
                        std::vector<ShardEndpoint*>* endpoints) const {
        return targetQuery(deleteDoc.getQuery(), endpoints);
    }

    Status targetCollection(std::vector<ShardEndpoint*>* endpoints) const {
        // TODO: XXX
        // No-op
        return Status::OK();
    }

    Status targetAllShards(std::vector<ShardEndpoint*>* endpoints) const {
        const std::vector<MockRange*>& ranges = getRanges();
        for (std::vector<MockRange*>::const_iterator it = ranges.begin(); it != ranges.end();
             ++it) {
            const MockRange* range = *it;
            endpoints->push_back(new ShardEndpoint(range->endpoint));
        }

        return Status::OK();
    }

    void noteCouldNotTarget() {
        // No-op
    }

    void noteStaleResponse(const ShardEndpoint& endpoint, const BSONObj& staleInfo) {
        // No-op
    }

    Status refreshIfNeeded(OperationContext* txn, bool* wasChanged) {
        // No-op
        if (wasChanged)
            *wasChanged = false;
        return Status::OK();
    }

    const std::vector<MockRange*>& getRanges() const {
        return _mockRanges.vector();
    }

private:
    KeyRange parseRange(const BSONObj& query) const {
        std::string fieldName = query.firstElement().fieldName();

        if (query.firstElement().isNumber()) {
            return KeyRange("",
                            BSON(fieldName << query.firstElement().numberInt()),
                            BSON(fieldName << query.firstElement().numberInt() + 1),
                            BSON(fieldName << 1));
        } else if (query.firstElement().type() == Object) {
            BSONObj queryRange = query.firstElement().Obj();

            ASSERT(!queryRange[GTE.l_].eoo());
            ASSERT(!queryRange[LT.l_].eoo());

            BSONObjBuilder minKeyB;
            minKeyB.appendAs(queryRange[GTE.l_], fieldName);
            BSONObjBuilder maxKeyB;
            maxKeyB.appendAs(queryRange[LT.l_], fieldName);

            return KeyRange("", minKeyB.obj(), maxKeyB.obj(), BSON(fieldName << 1));
        }

        ASSERT(false);
        return KeyRange("", BSONObj(), BSONObj(), BSONObj());
    }

    /**
     * Returns the first ShardEndpoint for the query from the mock ranges.  Only can handle
     * queries of the form { field : { $gte : <value>, $lt : <value> } }.
     */
    Status targetQuery(const BSONObj& query, std::vector<ShardEndpoint*>* endpoints) const {
        KeyRange queryRange = parseRange(query);

        const std::vector<MockRange*>& ranges = getRanges();
        for (std::vector<MockRange*>::const_iterator it = ranges.begin(); it != ranges.end();
             ++it) {
            const MockRange* range = *it;

            if (rangeOverlaps(queryRange.minKey,
                              queryRange.maxKey,
                              range->range.minKey,
                              range->range.maxKey)) {
                endpoints->push_back(new ShardEndpoint(range->endpoint));
            }
        }

        if (endpoints->empty())
            return Status(ErrorCodes::UnknownError, "no mock ranges found for query");
        return Status::OK();
    }

    NamespaceString _nss;

    // Manually-stored ranges
    OwnedPointerVector<MockRange> _mockRanges;
};

inline void assertEndpointsEqual(const ShardEndpoint& endpointA, const ShardEndpoint& endpointB) {
    ASSERT_EQUALS(endpointA.shardName, endpointB.shardName);
    ASSERT_EQUALS(endpointA.shardVersion.toLong(), endpointB.shardVersion.toLong());
    ASSERT_EQUALS(endpointA.shardVersion.epoch(), endpointB.shardVersion.epoch());
}

}  // namespace mongo
