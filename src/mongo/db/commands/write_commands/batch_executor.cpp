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

#include "mongo/db/commands/write_commands/batch_executor.h"

#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/write_concern.h"

namespace mongo {

    WriteBatchExecutor::WriteBatchExecutor(Client* client, OpCounters* opCounters, LastError* le)
        : _client(client)
        , _opCounters(opCounters)
        , _le(le) {}

    bool WriteBatchExecutor::executeBatch(const WriteBatch& writeBatch,
                                          string* errMsg,
                                          BSONObjBuilder* result) {
        Timer commandTimer;

        BSONArrayBuilder resultsArray;
        bool batchSuccess = applyWriteBatch(writeBatch, &resultsArray);
        result->append("resultsBatchSuccess", batchSuccess);
        result->append("results", resultsArray.arr());

        BSONObjBuilder writeConcernResults;
        Timer writeConcernTimer;

        // TODO Define final layout for write commands result object.

        bool writeConcernSuccess = waitForWriteConcern(writeBatch.getWriteConcern(),
                                                       !batchSuccess,
                                                       &writeConcernResults,
                                                       errMsg);
        if (!writeConcernSuccess) {
            return false;
        }

        const char *writeConcernErrField = writeConcernResults.asTempObj().getStringField("err");
        // TODO Should consider changing following existing strange behavior with GLE?
        // - {w:2} specified with batch where any op fails skips replication wait, yields success
        bool writeConcernFulfilled = !writeConcernErrField || strlen(writeConcernErrField) == 0;
        writeConcernResults.append("micros", static_cast<long long>(writeConcernTimer.micros()));
        writeConcernResults.append("ok", writeConcernFulfilled);
        result->append("writeConcernResults", writeConcernResults.obj());

        result->append("micros", static_cast<long long>(commandTimer.micros()));

        return true;
    }

    bool WriteBatchExecutor::applyWriteBatch(const WriteBatch& writeBatch,
                                             BSONArrayBuilder* resultsArray) {
        bool batchSuccess = true;
        for (size_t i = 0; i < writeBatch.getNumWriteItems(); ++i) {
            const WriteBatch::WriteItem& writeItem = writeBatch.getWriteItem(i);

            // All writes in the batch must be of the same type:
            dassert(writeBatch.getWriteType() == writeItem.getWriteType());

            BSONObjBuilder results;
            bool opSuccess = applyWriteItem(writeBatch.getNS(), writeItem, &results);
            resultsArray->append(results.obj());

            batchSuccess &= opSuccess;

            if (!opSuccess && !writeBatch.getContinueOnError()) {
                break;
            }
        }

        return batchSuccess;
    }

    namespace {

        // Translates write item type to wire protocol op code.
        // Helper for WriteBatchExecutor::applyWriteItem().
        int getOpCode(WriteBatch::WriteType writeType) {
            switch (writeType) {
            case WriteBatch::WRITE_INSERT:
                return dbInsert;
            case WriteBatch::WRITE_UPDATE:
                return dbUpdate;
            case WriteBatch::WRITE_DELETE:
                return dbDelete;
            }
            dassert(false);
            return 0;
        }

    } // namespace

    bool WriteBatchExecutor::applyWriteItem(const string& ns,
                                            const WriteBatch::WriteItem& writeItem,
                                            BSONObjBuilder* results) {
        // Clear operation's LastError before starting.
        _le->reset(true);

        uint64_t itemTimeMicros = 0;
        bool opSuccess = true;

        // Each write operation executes in its own PageFaultRetryableSection.  This means that
        // a single batch can throw multiple PageFaultException's, which is not the case for
        // other operations.
        PageFaultRetryableSection s;
        while (true) {
            try {
                // Execute the write item as a child operation of the current operation.
                CurOp childOp(_client, _client->curop());

                // TODO Modify CurOp "wrapped" constructor to take an opcode, so calling .reset()
                // is unneeded
                childOp.reset(_client->getRemote(), getOpCode(writeItem.getWriteType()));

                childOp.ensureStarted();
                OpDebug& opDebug = childOp.debug();
                opDebug.ns = ns;
                {
                    Client::WriteContext ctx(ns);

                    switch(writeItem.getWriteType()) {
                    case WriteBatch::WRITE_INSERT:
                        opSuccess = applyInsert(ns, writeItem, &childOp);
                        break;
                    case WriteBatch::WRITE_UPDATE:
                        opSuccess = applyUpdate(ns, writeItem, &childOp);
                        break;
                    case WriteBatch::WRITE_DELETE:
                        opSuccess = applyDelete(ns, writeItem, &childOp);
                        break;
                    }
                }
                childOp.done();
                itemTimeMicros = childOp.totalTimeMicros();

                opDebug.executionTime = childOp.totalTimeMillis();
                opDebug.recordStats();

                // Log operation if running with at least "-v", or if exceeds slow threshold.
                if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))
                    || opDebug.executionTime > cmdLine.slowMS + childOp.getExpectedLatencyMs()) {

                    MONGO_TLOG(1) << opDebug.report(childOp) << endl;
                }

                // TODO Log operation if logLevel >= 3 and assertion thrown (as assembleResponse()
                // does).

                // Save operation to system.profile if shouldDBProfile().
                if (childOp.shouldDBProfile(opDebug.executionTime)) {
                    profile(*_client, getOpCode(writeItem.getWriteType()), childOp);
                }
                break;
            }
            catch (PageFaultException& e) {
                e.touch();
            }
        }

        // Fill caller's builder with results of operation, using LastError.
        results->append("ok", opSuccess);
        _le->appendSelf(*results, false);
        results->append("micros", static_cast<long long>(itemTimeMicros));

        return opSuccess;
    }

    bool WriteBatchExecutor::applyInsert(const string& ns,
                                         const WriteBatch::WriteItem& writeItem,
                                         CurOp* currentOp) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotInsert();

        opDebug.op = dbInsert;

        BSONObj doc;

        string errMsg;
        bool ret = writeItem.parseInsertItem(&errMsg, &doc);
        verify(ret); // writeItem should have been already validated by WriteBatch::parse().

        try {
            // TODO Should call insertWithObjMod directly instead of checkAndInsert?  Note that
            // checkAndInsert will use mayInterrupt=false, so index builds initiated here won't
            // be interruptible.
            checkAndInsert(ns.c_str(), doc);
            getDur().commitIfNeeded();
            _le->nObjects = 1; // TODO Replace after implementing LastError::recordInsert().
            opDebug.ninserted = 1;
        }
        catch (UserException& e) {
            opDebug.exceptionInfo = e.getInfo();
            return false;
        }

        return true;
    }

    bool WriteBatchExecutor::applyUpdate(const string& ns,
                                         const WriteBatch::WriteItem& writeItem,
                                         CurOp* currentOp) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotUpdate();

        BSONObj queryObj;
        BSONObj updateObj;
        bool multi;
        bool upsert;

        string errMsg;
        bool ret = writeItem.parseUpdateItem(&errMsg, &queryObj, &updateObj, &multi, &upsert);
        verify(ret); // writeItem should have been already validated by WriteBatch::parse().

        currentOp->setQuery(queryObj);
        opDebug.op = dbUpdate;
        opDebug.query = queryObj;

        bool resExisting = false;
        long long resNum = 0;
        OID resUpserted = OID();
        try {

            const NamespaceString requestNs(ns);
            UpdateRequest request(requestNs);

            request.setQuery(queryObj);
            request.setUpdates(updateObj);
            request.setUpsert(upsert);
            request.setMulti(multi);
            request.setUpdateOpLog();

            UpdateResult res = update(request, &opDebug);

            resExisting = res.existing;
            resNum = res.numMatched;
            resUpserted = res.upserted;
        }
        catch (UserException& e) {
            opDebug.exceptionInfo = e.getInfo();
            return false;
        }

        _le->recordUpdate(resExisting, resNum, resUpserted);

        return true;
    }

    bool WriteBatchExecutor::applyDelete(const string& ns,
                                         const WriteBatch::WriteItem& writeItem,
                                         CurOp* currentOp) {
        OpDebug& opDebug = currentOp->debug();

        _opCounters->gotDelete();

        BSONObj queryObj;

        string errMsg;
        bool ret = writeItem.parseDeleteItem(&errMsg, &queryObj);
        verify(ret); // writeItem should have been already validated by WriteBatch::parse().

        currentOp->setQuery(queryObj);
        opDebug.op = dbDelete;
        opDebug.query = queryObj;

        long long n;

        try {
            n = deleteObjects(ns.c_str(),
                              queryObj,
                              /*justOne*/false,
                              /*logOp*/true,
                              /*god*/false);
        }
        catch (UserException& e) {
            opDebug.exceptionInfo = e.getInfo();
            return false;
        }

        _le->recordDelete(n);
        opDebug.ndeleted = n;

        return true;
    }

} // namespace mongo
