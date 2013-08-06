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

    // This is the only iteration method that should ever be called so it does all of the work.
    bool DocumentSourceOut::eof() {
        // make sure we only write out once
        if (_done)
            return true;
        _done = true;

        verify(_mongod);
        DBClientBase* conn = _mongod->directClient();

        prepTempCollection();
        verify(_tempNs.size() != 0);

        for (bool haveNext = !pSource->eof(); haveNext; haveNext = pSource->advance()) {
            BSONObj toInsert = pSource->getCurrent().toBson();
            conn->insert(_tempNs.ns(), toInsert);
            BSONObj err = conn->getLastErrorDetailed();
            uassert(16996, str::stream() << "insert for $out failed: " << err,
                    DBClientWithCommands::getLastErrorString(err).empty());
        }

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
        return true;
    }

    bool DocumentSourceOut::advance() {
        msgasserted(16998,
            "DocumentSourceOut::advance should never be called because eof() is always true.");
    }

    Document DocumentSourceOut::getCurrent() {
        msgasserted(16999,
            "DocumentSourceOut::getCurrent should never be called because eof() is always true.");
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
        
        NamespaceString outputNs(pExpCtx->getNs().db().toString() + '.' + pBsonElement->str());
        return new DocumentSourceOut(outputNs, pExpCtx);
    }

    void DocumentSourceOut::sourceToBson(BSONObjBuilder *pBuilder, bool explain) const {
        massert(17000, "$out shouldn't have different db than input",
                _outputNs.db() == pExpCtx->getNs().db());

        pBuilder->append("$out", _outputNs.coll());
    }
}
