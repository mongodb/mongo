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
            if (_conn && _tempNs.size())
                _conn->dropCollection(_tempNs.ns());
        )
    }

    const char *DocumentSourceOut::getSourceName() const {
        return outName;
    }

    void DocumentSourceOut::alterCommandResult(BSONObjBuilder& cmdResult) {
        cmdResult.append("outputNs", _db + '.' + _outputCollection);
    }

    static AtomicUInt32 aggOutCounter;
    void DocumentSourceOut::prepTempCollection(const string& finalNs) {
        verify(_conn);
        verify(_tempNs.size() == 0);

        _tempNs = StringData(str::stream() << _db
                                           << ".tmp.agg_out."
                                           << aggOutCounter.addAndFetch(1)
                                           );

        {
            BSONObj info;
            bool ok =_conn->runCommand(_db,
                                       BSON("create" << _tempNs.coll() << "temp" << true),
                                       info);
            uassert(16994, str::stream() << "failed to create temporary $out collection '"
                                         << _tempNs.ns() << "': " << info.toString(),
                    ok);
        }

        // copy indexes on finalNs to _tempNs
        scoped_ptr<DBClientCursor> indexes(_conn->getIndexes(finalNs));
        while (indexes->more()) {
            MutableDocument index(Document(indexes->nextSafe()));
            index.remove("_id"); // indexes shouldn't have _ids but some existing ones do
            index["ns"] = Value(_tempNs.ns());

            BSONObj indexBson = index.freeze().toBson();
            _conn->insert(_tempNs.getSystemIndexesCollection(), indexBson);
            BSONObj err = _conn->getLastErrorDetailed(_db);
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

        verify(_conn);

        const string finalNs = _db + '.' + _outputCollection;
        prepTempCollection(finalNs);
        verify(_tempNs.size() != 0);

        for (bool haveNext = !pSource->eof(); haveNext; haveNext = pSource->advance()) {
            BSONObj toInsert = pSource->getCurrent().toBson();
            _conn->insert(_tempNs.ns(), toInsert);
            BSONObj err = _conn->getLastErrorDetailed(_db);
            uassert(16996, str::stream() << "insert for $out failed: " << err,
                    DBClientWithCommands::getLastErrorString(err).empty());
        }

        BSONObj rename = BSON("renameCollection" << _tempNs.ns()
                           << "to" << finalNs
                           << "dropTarget" << true
                           );
        BSONObj info;
        bool ok = _conn->runCommand("admin", rename, info);
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

    DocumentSourceOut::DocumentSourceOut(StringData outputCollection,
                                         const intrusive_ptr<ExpressionContext>& pExpCtx)
        : SplittableDocumentSource(pExpCtx)
        , _done(false)
        , _tempNs("") // filled in by prepTempCollection
        , _outputCollection(outputCollection.toString())
    {}

    intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(16990, str::stream() << "$out only supports a string argument, not "
                                     << typeName(pBsonElement->type()),
                pBsonElement->type() == String);
        return new DocumentSourceOut(pBsonElement->str(), pExpCtx);
    }

    void DocumentSourceOut::sourceToBson(BSONObjBuilder *pBuilder, bool explain) const {
        pBuilder->append("$out", _outputCollection);
    }
}
