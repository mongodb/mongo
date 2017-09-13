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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_out.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/destructor_guard.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

DocumentSourceOut::~DocumentSourceOut() {
    DESTRUCTOR_GUARD(
        // Make sure we drop the temp collection if anything goes wrong. Errors are ignored
        // here because nothing can be done about them. Additionally, if this fails and the
        // collection is left behind, it will be cleaned up next time the server is started.
        if (_mongod && _tempNs.size()) _mongod->directClient()->dropCollection(_tempNs.ns());)
}

std::unique_ptr<LiteParsedDocumentSourceForeignCollections> DocumentSourceOut::liteParse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$out stage requires a string argument, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::String);

    NamespaceString targetNss(request.getNamespaceString().db(), spec.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid $out target namespace, " << targetNss.ns(),
            targetNss.isValid());

    ActionSet actions{ActionType::remove, ActionType::insert};
    if (request.shouldBypassDocumentValidation()) {
        actions.addAction(ActionType::bypassDocumentValidation);
    }

    PrivilegeVector privileges{Privilege(ResourcePattern::forExactNamespace(targetNss), actions)};

    return stdx::make_unique<LiteParsedDocumentSourceForeignCollections>(std::move(targetNss),
                                                                         std::move(privileges));
}

REGISTER_DOCUMENT_SOURCE(out, DocumentSourceOut::liteParse, DocumentSourceOut::createFromBson);

const char* DocumentSourceOut::getSourceName() const {
    return "$out";
}

static AtomicUInt32 aggOutCounter;

void DocumentSourceOut::initialize() {
    invariant(_mongod);
    DBClientBase* conn = _mongod->directClient();

    // Save the original collection options and index specs so we can check they didn't change
    // during computation.
    _originalOutOptions = _mongod->getCollectionOptions(_outputNs);
    _originalIndexes = conn->getIndexSpecs(_outputNs.ns());

    // Check if it's sharded or capped to make sure we have a chance of succeeding before we do all
    // the work. If the collection becomes capped during processing, the collection options will
    // have changed, and the $out will fail. If it becomes sharded during processing, the final
    // rename will fail.
    uassert(17017,
            str::stream() << "namespace '" << _outputNs.ns()
                          << "' is sharded so it can't be used for $out'",
            !_mongod->isSharded(_outputNs));
    uassert(17152,
            str::stream() << "namespace '" << _outputNs.ns()
                          << "' is capped so it can't be used for $out",
            _originalOutOptions["capped"].eoo());

    // We will write all results into a temporary collection, then rename the temporary collection
    // to be the target collection once we are done.
    _tempNs = NamespaceString(str::stream() << _outputNs.db() << ".tmp.agg_out."
                                            << aggOutCounter.addAndFetch(1));

    // Create output collection, copying options from existing collection if any.
    {
        BSONObjBuilder cmd;
        cmd << "create" << _tempNs.coll();
        cmd << "temp" << true;
        cmd.appendElementsUnique(_originalOutOptions);

        BSONObj info;
        bool ok = conn->runCommand(_outputNs.db().toString(), cmd.done(), info);
        uassert(16994,
                str::stream() << "failed to create temporary $out collection '" << _tempNs.ns()
                              << "': "
                              << info.toString(),
                ok);
    }

    // copy indexes to _tempNs
    for (std::list<BSONObj>::const_iterator it = _originalIndexes.begin();
         it != _originalIndexes.end();
         ++it) {
        MutableDocument index((Document(*it)));
        index.remove("_id");  // indexes shouldn't have _ids but some existing ones do
        index["ns"] = Value(_tempNs.ns());

        BSONObj indexBson = index.freeze().toBson();
        conn->insert(_tempNs.getSystemIndexesCollection(), indexBson);
        BSONObj err = conn->getLastErrorDetailed();
        uassert(16995,
                str::stream() << "copying index for $out failed."
                              << " index: "
                              << indexBson
                              << " error: "
                              << err,
                DBClientBase::getLastErrorString(err).empty());
    }
    _initialized = true;
}

void DocumentSourceOut::spill(const vector<BSONObj>& toInsert) {
    BSONObj err = _mongod->insert(_tempNs, toInsert);
    uassert(16996,
            str::stream() << "insert for $out failed: " << err,
            DBClientBase::getLastErrorString(err).empty());
}

DocumentSource::GetNextResult DocumentSourceOut::getNext() {
    pExpCtx->checkForInterrupt();

    if (_done) {
        return GetNextResult::makeEOF();
    }

    if (!_initialized) {
        initialize();
    }

    // Insert all documents into temp collection, batching to perform vectored inserts.
    vector<BSONObj> bufferedObjects;
    int bufferedBytes = 0;

    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        BSONObj toInsert = nextInput.releaseDocument().toBson();

        bufferedBytes += toInsert.objsize();
        if (!bufferedObjects.empty() && (bufferedBytes > BSONObjMaxUserSize ||
                                         bufferedObjects.size() >= write_ops::kMaxWriteBatchSize)) {
            spill(bufferedObjects);
            bufferedObjects.clear();
            bufferedBytes = toInsert.objsize();
        }
        bufferedObjects.push_back(toInsert);
    }
    if (!bufferedObjects.empty())
        spill(bufferedObjects);

    switch (nextInput.getStatus()) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case GetNextResult::ReturnStatus::kPauseExecution: {
            return nextInput;  // Propagate the pause.
        }
        case GetNextResult::ReturnStatus::kEOF: {

            auto renameCommandObj =
                BSON("renameCollection" << _tempNs.ns() << "to" << _outputNs.ns() << "dropTarget"
                                        << true);

            auto status = _mongod->renameIfOptionsAndIndexesHaveNotChanged(
                renameCommandObj, _outputNs, _originalOutOptions, _originalIndexes);
            uassert(16997, str::stream() << "$out failed: " << status.reason(), status.isOK());

            // We don't need to drop the temp collection in our destructor if the rename succeeded.
            _tempNs = {};
            _done = true;

            // $out doesn't currently produce any outputs.
            return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

DocumentSourceOut::DocumentSourceOut(const NamespaceString& outputNs,
                                     const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceNeedsMongod(pExpCtx),
      _done(false),
      _tempNs(""),  // Filled in during getNext().
      _outputNs(outputNs) {}

intrusive_ptr<DocumentSource> DocumentSourceOut::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(16990,
            str::stream() << "$out only supports a string argument, not " << typeName(elem.type()),
            elem.type() == String);

    uassert(ErrorCodes::InvalidOptions,
            "$out can only be used with the 'local' read concern level",
            !pExpCtx->opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot());

    NamespaceString outputNs(pExpCtx->ns.db().toString() + '.' + elem.str());
    uassert(17385, "Can't $out to special collection: " + elem.str(), !outputNs.isSpecial());
    return new DocumentSourceOut(outputNs, pExpCtx);
}

Value DocumentSourceOut::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    massert(
        17000, "$out shouldn't have different db than input", _outputNs.db() == pExpCtx->ns.db());

    return Value(DOC(getSourceName() << _outputNs.coll()));
}

DocumentSource::GetDepsReturn DocumentSourceOut::getDependencies(DepsTracker* deps) const {
    deps->needWholeDocument = true;
    return EXHAUSTIVE_ALL;
}
}
