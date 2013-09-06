/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

namespace mongo {
    const char DocumentSourceOut::outName[] = "$out";

    DocumentSourceOut::~DocumentSourceOut() {
        DESTRUCTOR_GUARD(
            // Make sure we drop the temp collection if anything goes wrong. Errors are ignored
            // here because nothing can be done about them. Additionally, if this fails and the
            // collection is left behind, it will be cleaned up next time the server is started.
            if (_mongod && _tempNs.size())
                _mongod->directClient()->dropCollection(_tempNs.ns());
        )
    }

    const char *DocumentSourceOut::getSourceName() const {
        return outName;
    }

    static AtomicUInt32 aggOutCounter;
    void DocumentSourceOut::prepTempCollection() {
        verify(_mongod);
        verify(_tempNs.size() == 0);

        DBClientBase* conn = _mongod->directClient();

        // Fail early by checking before we do any work.
        uassert(17017, str::stream() << "namespace '" << _outputNs.ns()
                                     << "' is sharded so it can't be used for $out'",
                !_mongod->isSharded(_outputNs));

        _tempNs = StringData(str::stream() << _outputNs.db()
                                           << ".tmp.agg_out."
                                           << aggOutCounter.addAndFetch(1)
                                           );

        {
            BSONObj info;
            bool ok =conn->runCommand(_outputNs.db().toString(),
                                      BSON("create" << _tempNs.coll() << "temp" << true),
                                      info);
            uassert(16994, str::stream() << "failed to create temporary $out collection '"
                                         << _tempNs.ns() << "': " << info.toString(),
                    ok);
        }

        // copy indexes on _outputNs to _tempNs
        scoped_ptr<DBClientCursor> indexes(conn->getIndexes(_outputNs));
        while (indexes->more()) {
            MutableDocument index(Document(indexes->nextSafe()));
            index.remove("_id"); // indexes shouldn't have _ids but some existing ones do
            index["ns"] = Value(_tempNs.ns());

            BSONObj indexBson = index.freeze().toBson();
            conn->insert(_tempNs.getSystemIndexesCollection(), indexBson);
            BSONObj err = conn->getLastErrorDetailed();
            uassert(16995, str::stream() << "copying index for $out failed."
                                         << " index: " << indexBson
                                         << " error: " <<  err,
                    DBClientWithCommands::getLastErrorString(err).empty());
        }
    }

    void DocumentSourceOut::spill(DBClientBase* conn, const vector<BSONObj>& toInsert) {
        conn->insert(_tempNs.ns(), toInsert);
        BSONObj err = conn->getLastErrorDetailed();
        uassert(16996, str::stream() << "insert for $out failed: " << err,
                DBClientWithCommands::getLastErrorString(err).empty());
    }

    boost::optional<Document> DocumentSourceOut::getNext() {
        pExpCtx->checkForInterrupt();

        // make sure we only write out once
        if (_done)
            return boost::none;
        _done = true;

        verify(_mongod);
        DBClientBase* conn = _mongod->directClient();

        prepTempCollection();
        verify(_tempNs.size() != 0);

        vector<BSONObj> bufferedObjects;
        int bufferedBytes = 0;
        while (boost::optional<Document> next = pSource->getNext()) {
            BSONObj toInsert = next->toBson();
            bufferedBytes += toInsert.objsize();
            if (!bufferedObjects.empty() && bufferedBytes > BSONObjMaxUserSize) {
                spill(conn, bufferedObjects);
                bufferedObjects.clear();
                bufferedBytes = toInsert.objsize();
            }
            bufferedObjects.push_back(toInsert);
        }

        if (!bufferedObjects.empty())
            spill(conn, bufferedObjects);

        // Checking again to make sure we didn't become sharded while running.
        uassert(17018, str::stream() << "namespace '" << _outputNs.ns()
                                     << "' became sharded so it can't be used for $out'",
                !_mongod->isSharded(_outputNs));

        BSONObj rename = BSON("renameCollection" << _tempNs.ns()
                           << "to" << _outputNs.ns()
                           << "dropTarget" << true
                           );
        BSONObj info;
        bool ok = conn->runCommand("admin", rename, info);
        uassert(16997,  str::stream() << "renameCollection for $out failed: " << info,
                ok);

        // We don't need to drop the temp collection in our destructor if the rename succeeded.
        _tempNs = NamespaceString("");

        // This "DocumentSource" doesn't produce output documents. This can change in the future
        // if we support using $out in "tee" mode.
        return boost::none;
    }

    DocumentSourceOut::DocumentSourceOut(const NamespaceString& outputNs,
                                         const intrusive_ptr<ExpressionContext>& pExpCtx)
        : SplittableDocumentSource(pExpCtx)
        , _done(false)
        , _tempNs("") // filled in by prepTempCollection
        , _outputNs(outputNs)
    {}

    intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(16990, str::stream() << "$out only supports a string argument, not "
                                     << typeName(pBsonElement->type()),
                pBsonElement->type() == String);
        
        NamespaceString outputNs(pExpCtx->ns.db().toString() + '.' + pBsonElement->str());
        return new DocumentSourceOut(outputNs, pExpCtx);
    }

    Value DocumentSourceOut::serialize(bool explain) const {
        massert(17000, "$out shouldn't have different db than input",
                _outputNs.db() == pExpCtx->ns.db());

        return Value(DOC(getSourceName() << _outputNs.coll()));
    }
}
