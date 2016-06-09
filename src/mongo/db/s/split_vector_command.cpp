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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

namespace {

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
}

class SplitVector : public Command {
public:
    SplitVector() : Command("splitVector", false) {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "Internal command.\n"
                "examples:\n"
                "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, "
                "maxChunkSize:200 }\n"
                "  maxChunkSize unit in MBs\n"
                "  May optionally specify 'maxSplitPoints' and 'maxChunkObjects' to avoid "
                "traversing the whole chunk\n"
                "  \n"
                "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, "
                "force: true }\n"
                "  'force' will produce one split point even if data is small; defaults to false\n"
                "NOTE: This command may take a while to run";
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    virtual std::string parseNs(const string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        //
        // 1.a We'll parse the parameters in two steps. First, make sure the we can use the split
        //     index to get a good approximation of the size of the chunk -- without needing to
        //     access the actual data.
        //

        const NamespaceString nss = NamespaceString(parseNs(dbname, jsobj));
        BSONObj keyPattern = jsobj.getObjectField("keyPattern");

        if (keyPattern.isEmpty()) {
            errmsg = "no key pattern found in splitVector";
            return false;
        }

        // If min and max are not provided use the "minKey" and "maxKey" for the sharding key
        // pattern.
        BSONObj min = jsobj.getObjectField("min");
        BSONObj max = jsobj.getObjectField("max");
        if (min.isEmpty() != max.isEmpty()) {
            errmsg = "either provide both min and max or leave both empty";
            return false;
        }

        long long maxSplitPoints = 0;
        BSONElement maxSplitPointsElem = jsobj["maxSplitPoints"];
        if (maxSplitPointsElem.isNumber()) {
            maxSplitPoints = maxSplitPointsElem.numberLong();
        }

        long long maxChunkObjects = Chunk::MaxObjectPerChunk;
        BSONElement MaxChunkObjectsElem = jsobj["maxChunkObjects"];
        if (MaxChunkObjectsElem.isNumber()) {
            maxChunkObjects = MaxChunkObjectsElem.numberLong();
        }

        vector<BSONObj> splitKeys;

        {
            // Get the size estimate for this namespace
            AutoGetCollection autoColl(txn, nss, MODE_IS);

            Collection* const collection = autoColl.getCollection();
            if (!collection) {
                errmsg = "ns not found";
                return false;
            }

            // Allow multiKey based on the invariant that shard keys must be single-valued.
            // Therefore, any multi-key index prefixed by shard key cannot be multikey over
            // the shard key fields.
            IndexDescriptor* idx =
                collection->getIndexCatalog()->findShardKeyPrefixedIndex(txn, keyPattern, false);
            if (idx == NULL) {
                errmsg = (string) "couldn't find index over splitting key " +
                    keyPattern.clientReadable().toString();
                return false;
            }
            // extend min to get (min, MinKey, MinKey, ....)
            KeyPattern kp(idx->keyPattern());
            min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
            if (max.isEmpty()) {
                // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                max = Helpers::toKeyFormat(kp.extendRangeBound(max, true));
            } else {
                // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));
            }

            const long long recCount = collection->numRecords(txn);
            const long long dataSize = collection->dataSize(txn);

            //
            // 1.b Now that we have the size estimate, go over the remaining parameters and apply
            //      any maximum size restrictions specified there.
            //

            // 'force'-ing a split is equivalent to having maxChunkSize be the size of the current
            // chunk, i.e., the logic below will split that chunk in half
            long long maxChunkSize = 0;
            bool forceMedianSplit = false;
            {
                BSONElement maxSizeElem = jsobj["maxChunkSize"];
                BSONElement forceElem = jsobj["force"];

                if (forceElem.trueValue()) {
                    forceMedianSplit = true;
                    // This chunk size is effectively ignored if force is true
                    maxChunkSize = dataSize;

                } else if (maxSizeElem.isNumber()) {
                    maxChunkSize = maxSizeElem.numberLong() * 1 << 20;

                } else {
                    maxSizeElem = jsobj["maxChunkSizeBytes"];
                    if (maxSizeElem.isNumber()) {
                        maxChunkSize = maxSizeElem.numberLong();
                    }
                }

                // We need a maximum size for the chunk, unless we're not actually capable of
                // finding any split points.
                if (maxChunkSize <= 0 && recCount != 0) {
                    errmsg =
                        "need to specify the desired max chunk size (maxChunkSize or "
                        "maxChunkSizeBytes)";
                    return false;
                }
            }


            // If there's not enough data for more than one chunk, no point continuing.
            if (dataSize < maxChunkSize || recCount == 0) {
                vector<BSONObj> emptyVector;
                result.append("splitKeys", emptyVector);
                return true;
            }

            log() << "request split points lookup for chunk " << nss.toString() << " " << min
                  << " -->> " << max;

            // We'll use the average object size and number of object to find approximately how many
            // keys each chunk should have. We'll split at half the maxChunkSize or maxChunkObjects,
            // if provided.
            const long long avgRecSize = dataSize / recCount;
            long long keyCount = maxChunkSize / (2 * avgRecSize);
            if (maxChunkObjects && (maxChunkObjects < keyCount)) {
                log() << "limiting split vector to " << maxChunkObjects << " (from " << keyCount
                      << ") objects ";
                keyCount = maxChunkObjects;
            }

            //
            // 2. Traverse the index and add the keyCount-th key to the result vector. If that key
            //    appeared in the vector before, we omit it. The invariant here is that all the
            //    instances of a given key value live in the same chunk.
            //

            Timer timer;
            long long currCount = 0;
            long long numChunks = 0;

            unique_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn,
                                                                     collection,
                                                                     idx,
                                                                     min,
                                                                     max,
                                                                     false,  // endKeyInclusive
                                                                     PlanExecutor::YIELD_MANUAL,
                                                                     InternalPlanner::FORWARD));

            BSONObj currKey;
            PlanExecutor::ExecState state = exec->getNext(&currKey, NULL);
            if (PlanExecutor::ADVANCED != state) {
                errmsg = "can't open a cursor for splitting (desired range is possibly empty)";
                return false;
            }

            // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel,
            // to be removed at the end. If a key appears more times than entries allowed on a
            // chunk, we issue a warning and split on the following key.
            set<BSONObj> tooFrequentKeys;
            splitKeys.push_back(dps::extractElementsBasedOnTemplate(
                prettyKey(idx->keyPattern(), currKey.getOwned()), keyPattern));

            exec->setYieldPolicy(PlanExecutor::YIELD_AUTO, collection);
            while (1) {
                while (PlanExecutor::ADVANCED == state) {
                    currCount++;

                    if (currCount > keyCount && !forceMedianSplit) {
                        currKey = dps::extractElementsBasedOnTemplate(
                            prettyKey(idx->keyPattern(), currKey.getOwned()), keyPattern);
                        // Do not use this split key if it is the same used in the previous split
                        // point.
                        if (currKey.woCompare(splitKeys.back()) == 0) {
                            tooFrequentKeys.insert(currKey.getOwned());
                        } else {
                            splitKeys.push_back(currKey.getOwned());
                            currCount = 0;
                            numChunks++;
                            LOG(4) << "picked a split key: " << currKey;
                        }
                    }

                    // Stop if we have enough split points.
                    if (maxSplitPoints && (numChunks >= maxSplitPoints)) {
                        log() << "max number of requested split points reached (" << numChunks
                              << ") before the end of chunk " << nss.toString() << " " << min
                              << " -->> " << max;
                        break;
                    }

                    state = exec->getNext(&currKey, NULL);
                }

                if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
                    return appendCommandStatus(
                        result,
                        Status(ErrorCodes::OperationFailed,
                               str::stream() << "Executor error during splitVector command: "
                                             << WorkingSetCommon::toStatusString(currKey)));
                }

                if (!forceMedianSplit)
                    break;

                //
                // If we're forcing a split at the halfway point, then the first pass was just
                // to count the keys, and we still need a second pass.
                //

                forceMedianSplit = false;
                keyCount = currCount / 2;
                currCount = 0;
                log() << "splitVector doing another cycle because of force, keyCount now: "
                      << keyCount;

                exec = InternalPlanner::indexScan(txn,
                                                  collection,
                                                  idx,
                                                  min,
                                                  max,
                                                  false,  // endKeyInclusive
                                                  PlanExecutor::YIELD_MANUAL,
                                                  InternalPlanner::FORWARD);

                exec->setYieldPolicy(PlanExecutor::YIELD_AUTO, collection);
                state = exec->getNext(&currKey, NULL);
            }

            //
            // 3. Format the result and issue any warnings about the data we gathered while
            //    traversing the index
            //

            // Warn for keys that are more numerous than maxChunkSize allows.
            for (set<BSONObj>::const_iterator it = tooFrequentKeys.begin();
                 it != tooFrequentKeys.end();
                 ++it) {
                warning() << "possible low cardinality key detected in " << nss.toString()
                          << " - key is " << prettyKey(idx->keyPattern(), *it);
            }

            // Remove the sentinel at the beginning before returning
            splitKeys.erase(splitKeys.begin());

            if (timer.millis() > serverGlobalParams.slowMS) {
                warning() << "Finding the split vector for " << nss.toString() << " over "
                          << keyPattern << " keyCount: " << keyCount
                          << " numSplits: " << splitKeys.size() << " lookedAt: " << currCount
                          << " took " << timer.millis() << "ms";
            }

            // Warning: we are sending back an array of keys but are currently limited to
            // 4MB work of 'result' size. This should be okay for now.

            result.append("timeMillis", timer.millis());
        }

        // Make sure splitKeys is in ascending order
        std::sort(splitKeys.begin(),
                  splitKeys.end(),
                  [](const BSONObj& lhs, const BSONObj& rhs) -> bool { return lhs < rhs; });
        result.append("splitKeys", splitKeys);
        return true;
    }

} cmdSplitVector;

}  // namespace
}  // namespace mongo
