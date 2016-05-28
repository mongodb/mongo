/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::set;
using std::string;
using std::vector;

class TestableChunkManager : public ChunkManager {
public:
    TestableChunkManager(const string& ns, const ShardKeyPattern& keyPattern, bool unique)
        : ChunkManager(ns, keyPattern, unique) {}

    void setSingleChunkForShards(const vector<BSONObj>& splitPoints) {
        vector<BSONObj> mySplitPoints(splitPoints);
        mySplitPoints.insert(mySplitPoints.begin(), _keyPattern.getKeyPattern().globalMin());
        mySplitPoints.push_back(_keyPattern.getKeyPattern().globalMax());

        for (unsigned i = 1; i < mySplitPoints.size(); ++i) {
            const string shardId = str::stream() << (i - 1);
            _shardIds.insert(shardId);

            std::shared_ptr<Chunk> chunk(new Chunk(this,
                                                   mySplitPoints[i - 1],
                                                   mySplitPoints[i],
                                                   shardId,
                                                   ChunkVersion(0, 0, OID()),
                                                   0));
            _chunkMap[mySplitPoints[i]] = chunk;
        }

        _chunkRangeMap = _constructRanges(_chunkMap);
    }
};

namespace {

class Base {
public:
    Base() = default;

    virtual ~Base() = default;

    void run() {
        auto opCtx = stdx::make_unique<OperationContextNoop>();

        ShardKeyPattern shardKeyPattern(shardKey());
        TestableChunkManager chunkManager("", shardKeyPattern, false);
        chunkManager.setSingleChunkForShards(splitPointsVector());

        set<ShardId> shardIds;
        chunkManager.getShardIdsForQuery(opCtx.get(), query(), &shardIds);

        BSONArrayBuilder b;
        for (const ShardId& shardId : shardIds) {
            b << shardId;
        }
        ASSERT_EQUALS(expectedShardNames(), b.arr());
    }

protected:
    virtual BSONObj shardKey() const {
        return BSON("a" << 1);
    }

    virtual BSONArray splitPoints() const {
        return BSONArray();
    }

    virtual BSONObj query() const {
        return BSONObj();
    }

    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0");
    }

    virtual vector<BSONObj> splitPointsVector() const {
        vector<BSONObj> ret;
        BSONArray a = splitPoints();
        BSONObjIterator i(a);
        while (i.more()) {
            ret.push_back(i.next().Obj().getOwned());
        }
        return ret;
    }
};

class EmptyQuerySingleShard : public Base {};

class MultiShardBase : public Base {
    virtual BSONArray splitPoints() const {
        return BSON_ARRAY(BSON("a"
                               << "x")
                          << BSON("a"
                                  << "y")
                          << BSON("a"
                                  << "z"));
    }
};

class EmptyQueryMultiShard : public MultiShardBase {
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "1"
                          << "2"
                          << "3");
    }
};

class UniversalRangeMultiShard : public EmptyQueryMultiShard {
    virtual BSONObj query() const {
        return BSON("b" << 1);
    }
};

class EqualityRangeSingleShard : public EmptyQuerySingleShard {
    virtual BSONObj query() const {
        return BSON("a"
                    << "x");
    }
};

class EqualityRangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return BSON("a"
                    << "y");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("2");
    }
};

class SetRangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return fromjson("{a:{$in:['u','y']}}");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "2");
    }
};

class GTRangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return BSON("a" << GT << "x");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("1"
                          << "2"
                          << "3");
    }
};

class GTERangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return BSON("a" << GTE << "x");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("1"
                          << "2"
                          << "3");
    }
};

class LTRangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return BSON("a" << LT << "y");
    }

    /**
     * It isn't actually necessary to return shard 2 because its lowest key is "y", which
     * is excluded from the query.  SERVER-4791
     */
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "1"
                          << "2");
    }
};

class LTERangeMultiShard : public MultiShardBase {
    virtual BSONObj query() const {
        return BSON("a" << LTE << "y");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "1"
                          << "2");
    }
};

class OrEqualities : public MultiShardBase {
    virtual BSONObj query() const {
        return fromjson("{$or:[{a:'u'},{a:'y'}]}");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "2");
    }
};

class OrEqualityInequality : public MultiShardBase {
    virtual BSONObj query() const {
        return fromjson("{$or:[{a:'u'},{a:{$gte:'y'}}]}");
    }
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "2"
                          << "3");
    }
};

class OrEqualityInequalityUnhelpful : public MultiShardBase {
    virtual BSONObj query() const {
        return fromjson("{$or:[{a:'u'},{a:{$gte:'zz'}},{}]}");
    }

    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "1"
                          << "2"
                          << "3");
    }
};

template <class BASE>
class Unsatisfiable : public BASE {
    /**
     * SERVER-4914 For now the first shard is returned for unsatisfiable queries, as some
     * clients of getShardIdsForQuery() expect at least one shard.
     */
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0");
    }
};

class UnsatisfiableRangeSingleShard : public Unsatisfiable<Base> {
    virtual BSONObj query() const {
        return BSON("a" << GT << "x" << LT << "x");
    }
};

class UnsatisfiableRangeMultiShard : public Unsatisfiable<MultiShardBase> {
    virtual BSONObj query() const {
        return BSON("a" << GT << "x" << LT << "x");
    }
};

class EqualityThenUnsatisfiable : public Unsatisfiable<Base> {
    virtual BSONObj shardKey() const {
        return BSON("a" << 1 << "b" << 1);
    }
    virtual BSONObj query() const {
        return BSON("a" << 1 << "b" << GT << 4 << LT << 4);
    }
};

class InequalityThenUnsatisfiable : public Unsatisfiable<Base> {
    virtual BSONObj shardKey() const {
        return BSON("a" << 1 << "b" << 1);
    }
    virtual BSONObj query() const {
        return BSON("a" << GT << 1 << "b" << GT << 4 << LT << 4);
    }
};

class OrEqualityUnsatisfiableInequality : public MultiShardBase {
    virtual BSONObj query() const {
        return fromjson("{$or:[{a:'x'},{a:{$gt:'u',$lt:'u'}},{a:{$gte:'y'}}]}");
    }

    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("1"
                          << "2"
                          << "3");
    }
};

class CompoundKeyBase : public Base {
    virtual BSONObj shardKey() const {
        return BSON("a" << 1 << "b" << 1);
    }

    virtual BSONArray splitPoints() const {
        return BSON_ARRAY(BSON("a" << 5 << "b" << 10) << BSON("a" << 5 << "b" << 20));
    }
};

class InMultiShard : public CompoundKeyBase {
    virtual BSONObj query() const {
        return BSON("a" << BSON("$in" << BSON_ARRAY(0 << 5 << 10)) << "b"
                        << BSON("$in" << BSON_ARRAY(0 << 5 << 25)));
    }

    // If we were to send this query to just the shards it actually needed to hit, it would
    // only hit shards 0 and 2. Because of the optimization from SERVER-4745, however, we'll
    // also hit shard 1.
    virtual BSONArray expectedShardNames() const {
        return BSON_ARRAY("0"
                          << "1"
                          << "2");
    }
};

class All : public Suite {
public:
    All() : Suite("chunk") {}

    void setupTests() {
        add<EmptyQuerySingleShard>();
        add<EmptyQueryMultiShard>();
        add<UniversalRangeMultiShard>();
        add<EqualityRangeSingleShard>();
        add<EqualityRangeMultiShard>();
        add<SetRangeMultiShard>();
        add<GTRangeMultiShard>();
        add<GTERangeMultiShard>();
        add<LTRangeMultiShard>();
        add<LTERangeMultiShard>();
        add<OrEqualities>();
        add<OrEqualityInequality>();
        add<OrEqualityInequalityUnhelpful>();
        add<UnsatisfiableRangeSingleShard>();
        add<UnsatisfiableRangeMultiShard>();
        add<EqualityThenUnsatisfiable>();
        add<InequalityThenUnsatisfiable>();
        add<OrEqualityUnsatisfiableInequality>();
        add<InMultiShard>();
    }
};

SuiteInstance<All> myAll;

}  // namespace
}  // namespace mongo
