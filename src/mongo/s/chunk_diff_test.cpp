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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_diff.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;
using std::pair;
using std::make_pair;
using std::map;
using std::vector;

// Generates pseudorandom values
PseudoRandom rand(1);

/**
 * The default pass-through adapter for using config diffs.
 */
class DefaultDiffAdapter : public ConfigDiffTracker<BSONObj> {
public:
    DefaultDiffAdapter() {}
    virtual ~DefaultDiffAdapter() {}

    virtual bool isTracked(const ChunkType& chunk) const {
        return true;
    }

    virtual pair<BSONObj, BSONObj> rangeFor(OperationContext* txn, const ChunkType& chunk) const {
        return make_pair(chunk.getMin(), chunk.getMax());
    }

    virtual ShardId shardFor(OperationContext* txn, const ShardId& name) const {
        return name;
    }
};

/**
 * Inverts the storage order for chunks from min to max.
 */
class InverseDiffAdapter : public DefaultDiffAdapter {
public:
    InverseDiffAdapter() {}
    virtual ~InverseDiffAdapter() {}

    virtual bool isMinKeyIndexed() const {
        return false;
    }

    virtual pair<BSONObj, BSONObj> rangeFor(OperationContext* txn, const ChunkType& chunk) const {
        return make_pair(chunk.getMax(), chunk.getMin());
    }
};

/**
 * Converts array of raw BSONObj chunks to a vector of ChunkType.
 */
void convertBSONArrayToChunkTypes(const vector<BSONObj>& chunksArray,
                                  std::vector<ChunkType>* chunksVector) {
    for (const BSONObj& obj : chunksArray) {
        auto chunkTypeRes = ChunkType::fromBSON(obj);
        ASSERT(chunkTypeRes.isOK());
        chunksVector->push_back(chunkTypeRes.getValue());
    }
}

class ChunkDiffUnitTest : public mongo::unittest::Test {
protected:
    typedef map<BSONObj, BSONObj, BSONObjCmp> RangeMap;
    typedef map<ShardId, ChunkVersion> VersionMap;

    ChunkDiffUnitTest() = default;
    ~ChunkDiffUnitTest() = default;

    void runTest(bool isInverse) {
        int numShards = 10;
        int numInitialChunks = 5;

        // Needed to not overflow the BSONArray's max bytes
        int maxChunks = 100000;
        int keySize = 2;

        vector<BSONObj> chunksB;

        BSONObj lastSplitPt;
        ChunkVersion version(1, 0, OID());

        // Generate numChunks with a given key size over numShards. All chunks have double key
        // values, so we can split them a bunch.

        for (int i = -1; i < numInitialChunks; i++) {
            BSONObjBuilder splitPtB;
            for (int k = 0; k < keySize; k++) {
                string field = string("k") + string(1, (char)('0' + k));
                if (i < 0)
                    splitPtB.appendMinKey(field);
                else if (i < numInitialChunks - 1)
                    splitPtB.append(field, (double)i);
                else
                    splitPtB.appendMaxKey(field);
            }
            BSONObj splitPt = splitPtB.obj();

            if (i >= 0) {
                BSONObjBuilder chunkB;

                chunkB.append(ChunkType::name(), "$dummyname");
                chunkB.append(ChunkType::ns(), "$dummyns");

                chunkB.append(ChunkType::min(), lastSplitPt);
                chunkB.append(ChunkType::max(), splitPt);

                int shardNum = rand(numShards);
                chunkB.append(ChunkType::shard(), "shard" + string(1, (char)('A' + shardNum)));

                rand(2) ? version.incMajor() : version.incMinor();
                version.addToBSON(chunkB, ChunkType::DEPRECATED_lastmod());

                chunksB.push_back(chunkB.obj());
            }

            lastSplitPt = splitPt;
        }

        vector<BSONObj> chunks(std::move(chunksB));

        // Setup the empty ranges and versions first
        RangeMap ranges;
        ChunkVersion maxVersion = ChunkVersion(0, 0, OID());
        VersionMap maxShardVersions;

        // Create a differ which will track our progress
        std::shared_ptr<DefaultDiffAdapter> differ(isInverse ? new InverseDiffAdapter()
                                                             : new DefaultDiffAdapter());
        differ->attach("test", ranges, maxVersion, maxShardVersions);

        std::vector<ChunkType> chunksVector;
        convertBSONArrayToChunkTypes(chunks, &chunksVector);

        // Validate initial load
        differ->calculateConfigDiff(nullptr, chunksVector);
        validate(isInverse, chunksVector, ranges, maxVersion, maxShardVersions);

        // Generate a lot of diffs, and keep validating that updating from the diffs always gives us
        // the right ranges and versions

        // Makes about 100000 chunks overall
        int numDiffs = 135;
        int numChunks = numInitialChunks;

        for (int i = 0; i < numDiffs; i++) {
            vector<BSONObj> newChunksB;

            vector<BSONObj>::iterator it = chunks.begin();

            while (it != chunks.end()) {
                BSONObj chunk = *it;
                ++it;

                int randChoice = rand(10);

                if (randChoice < 2 && numChunks < maxChunks) {
                    // Simulate a split
                    BSONObjBuilder leftB;
                    BSONObjBuilder rightB;
                    BSONObjBuilder midB;

                    for (int k = 0; k < keySize; k++) {
                        string field = string("k") + string(1, (char)('0' + k));

                        BSONType maxType = chunk[ChunkType::max()].Obj()[field].type();
                        double max =
                            maxType == NumberDouble ? chunk["max"].Obj()[field].Number() : 0.0;
                        BSONType minType = chunk[ChunkType::min()].Obj()[field].type();
                        double min = minType == NumberDouble
                            ? chunk[ChunkType::min()].Obj()[field].Number()
                            : 0.0;

                        if (minType == MinKey) {
                            midB.append(field, max - 1.0);
                        } else if (maxType == MaxKey) {
                            midB.append(field, min + 1.0);
                        } else {
                            midB.append(field, (max + min) / 2.0);
                        }
                    }

                    BSONObj midPt = midB.obj();

                    // Only happens if we can't split the min chunk
                    if (midPt.isEmpty()) {
                        continue;
                    }

                    leftB.append(chunk[ChunkType::min()]);
                    leftB.append(ChunkType::max(), midPt);
                    rightB.append(ChunkType::min(), midPt);
                    rightB.append(chunk[ChunkType::max()]);

                    // Add required fields for ChunkType
                    leftB.append(chunk[ChunkType::name()]);
                    leftB.append(chunk[ChunkType::ns()]);
                    rightB.append(chunk[ChunkType::name()]);
                    rightB.append(chunk[ChunkType::ns()]);

                    leftB.append(chunk[ChunkType::shard()]);
                    rightB.append(chunk[ChunkType::shard()]);

                    version.incMajor();
                    version.addToBSON(leftB, ChunkType::DEPRECATED_lastmod());
                    version.incMinor();
                    version.addToBSON(rightB, ChunkType::DEPRECATED_lastmod());

                    BSONObj left = leftB.obj();
                    BSONObj right = rightB.obj();

                    newChunksB.push_back(left);
                    newChunksB.push_back(right);

                    numChunks++;
                } else if (randChoice < 4 && it != chunks.end()) {
                    // Simulate a migrate
                    BSONObj prevShardChunk;
                    while (it != chunks.end()) {
                        prevShardChunk = *it;
                        ++it;

                        if (prevShardChunk[ChunkType::shard()].String() ==
                            chunk[ChunkType::shard()].String()) {
                            break;
                        }

                        newChunksB.push_back(prevShardChunk);

                        prevShardChunk = BSONObj();
                    }

                    // We need to move between different shards, hence the weirdness in logic here
                    if (!prevShardChunk.isEmpty()) {
                        BSONObjBuilder newShardB;
                        BSONObjBuilder prevShardB;

                        newShardB.append(chunk[ChunkType::min()]);
                        newShardB.append(chunk[ChunkType::max()]);
                        prevShardB.append(prevShardChunk[ChunkType::min()]);
                        prevShardB.append(prevShardChunk[ChunkType::max()]);

                        // add required fields for ChunkType
                        newShardB.append(chunk[ChunkType::name()]);
                        newShardB.append(chunk[ChunkType::ns()]);
                        prevShardB.append(chunk[ChunkType::name()]);
                        prevShardB.append(chunk[ChunkType::ns()]);

                        int shardNum = rand(numShards);
                        newShardB.append(ChunkType::shard(),
                                         "shard" + string(1, (char)('A' + shardNum)));
                        prevShardB.append(prevShardChunk[ChunkType::shard()]);

                        version.incMajor();
                        version.addToBSON(newShardB, ChunkType::DEPRECATED_lastmod());
                        version.incMinor();
                        version.addToBSON(prevShardB, ChunkType::DEPRECATED_lastmod());

                        BSONObj newShard = newShardB.obj();
                        BSONObj prevShard = prevShardB.obj();

                        newChunksB.push_back(newShard);
                        newChunksB.push_back(prevShard);
                    } else {
                        newChunksB.push_back(chunk);
                    }
                } else {
                    newChunksB.push_back(chunk);
                }
            }

            chunks = std::move(newChunksB);

            // Rarely entirely clear out our data
            if (rand(10) < 1) {
                ranges.clear();
                maxVersion = ChunkVersion(0, 0, OID());
                maxShardVersions.clear();
            }

            std::vector<ChunkType> chunksVector;
            convertBSONArrayToChunkTypes(chunks, &chunksVector);

            differ->calculateConfigDiff(nullptr, chunksVector);

            validate(isInverse, chunksVector, ranges, maxVersion, maxShardVersions);
        }
    }

private:
    // Allow validating with and without ranges (b/c our splits won't actually be updated by the
    // diffs)
    void validate(bool isInverse,
                  const std::vector<ChunkType>& chunks,
                  ChunkVersion maxVersion,
                  const VersionMap& maxShardVersions) {
        validate(isInverse, chunks, NULL, maxVersion, maxShardVersions);
    }

    void validate(bool isInverse,
                  const std::vector<ChunkType>& chunks,
                  const RangeMap& ranges,
                  ChunkVersion maxVersion,
                  const VersionMap& maxShardVersions) {
        validate(isInverse, chunks, (RangeMap*)&ranges, maxVersion, maxShardVersions);
    }

    // Validates that the ranges and versions are valid given the chunks
    void validate(bool isInverse,
                  const std::vector<ChunkType>& chunks,
                  RangeMap* ranges,
                  ChunkVersion maxVersion,
                  const VersionMap& maxShardVersions) {
        int chunkCount = chunks.size();
        ChunkVersion foundMaxVersion;
        VersionMap foundMaxShardVersions;

        //
        // Validate that all the chunks are there and collect versions
        //

        for (const ChunkType& chunk : chunks) {
            if (ranges != NULL) {
                // log() << "Validating chunk " << chunkDoc << " size : " << ranges->size() << " vs
                // " << chunkCount << endl;

                RangeMap::iterator chunkRange =
                    ranges->find(isInverse ? chunk.getMax() : chunk.getMin());

                ASSERT(chunkRange != ranges->end());
                ASSERT(chunkRange->second.woCompare(isInverse ? chunk.getMin() : chunk.getMax()) ==
                       0);
            }

            ChunkVersion version = chunk.getVersion();
            if (version > foundMaxVersion)
                foundMaxVersion = version;

            ChunkVersion shardMaxVersion = foundMaxShardVersions[chunk.getShard()];
            if (version > shardMaxVersion) {
                foundMaxShardVersions[chunk.getShard()] = version;
            }
        }

        // Make sure all chunks are accounted for
        if (ranges != NULL)
            ASSERT(chunkCount == (int)ranges->size());

        // log() << "Validating that all shard versions are up to date..." << endl;

        // Validate that all the versions are the same
        ASSERT(foundMaxVersion.equals(maxVersion));

        for (VersionMap::iterator it = foundMaxShardVersions.begin();
             it != foundMaxShardVersions.end();
             it++) {
            ChunkVersion foundVersion = it->second;
            VersionMap::const_iterator maxIt = maxShardVersions.find(it->first);

            ASSERT(maxIt != maxShardVersions.end());
            ASSERT(foundVersion.equals(maxIt->second));
        }
        // Make sure all shards are accounted for
        ASSERT(foundMaxShardVersions.size() == maxShardVersions.size());
    }
};

TEST_F(ChunkDiffUnitTest, Normal) {
    runTest(false);
}

TEST_F(ChunkDiffUnitTest, Inverse) {
    runTest(true);
}

}  // namespace
}  // namespace mongo
