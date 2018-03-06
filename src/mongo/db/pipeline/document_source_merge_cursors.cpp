/**
 * Copyright (C) 2018 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(mergeCursors,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceMergeCursors::createFromBson);

constexpr StringData DocumentSourceMergeCursors::kStageName;

DocumentSourceMergeCursors::DocumentSourceMergeCursors(
    executor::TaskExecutor* executor,
    std::unique_ptr<ClusterClientCursorParams> cursorParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(expCtx), _executor(executor), _armParams(std::move(cursorParams)) {}

DocumentSource::GetNextResult DocumentSourceMergeCursors::getNext() {
    // We don't expect or support tailable cursors to be executing through this stage.
    invariant(pExpCtx->tailableMode == TailableMode::kNormal);
    if (!_arm) {
        _arm.emplace(pExpCtx->opCtx, _executor, _armParams.get());
    }
    auto next = uassertStatusOK(_arm->blockingNext());
    if (next.isEOF()) {
        return GetNextResult::makeEOF();
    }
    return Document::fromBsonWithMetaData(*next.getResult());
}

void DocumentSourceMergeCursors::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    invariant(!_arm);
    invariant(_armParams);
    std::vector<Value> cursors;
    for (auto&& remote : _armParams->remotes) {
        cursors.emplace_back(Document{{"host", remote.hostAndPort.toString()},
                                      {"ns", remote.cursorResponse.getNSS().toString()},
                                      {"id", remote.cursorResponse.getCursorId()}});
    }
    array.push_back(Value(Document{{kStageName, Value(std::move(cursors))}}));
    if (!_armParams->sort.isEmpty()) {
        array.push_back(Value(Document{{DocumentSourceSort::kStageName, Value(_armParams->sort)}}));
    }
}

Pipeline::SourceContainer::iterator DocumentSourceMergeCursors::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);
    invariant(!_arm);
    invariant(_armParams);

    auto next = std::next(itr);
    if (next == container->end()) {
        return next;
    }

    auto nextSort = dynamic_cast<DocumentSourceSort*>(next->get());
    if (!nextSort || !nextSort->mergingPresorted()) {
        return next;
    }

    _armParams->sort =
        nextSort->sortKeyPattern(DocumentSourceSort::SortKeySerialization::kForSortKeyMerging)
            .toBson();
    if (auto sortLimit = nextSort->getLimitSrc()) {
        // There was a limit stage absorbed into the sort stage, so we need to preserve that.
        container->insert(std::next(next), sortLimit);
    }
    container->erase(next);
    return std::next(itr);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(
        17026, "$mergeCursors stage expected array as argument", elem.type() == BSONType::Array);
    const auto serializedRemotes = elem.Array();
    uassert(50729,
            "$mergeCursors stage expected array with at least one entry",
            serializedRemotes.size() > 0);

    boost::optional<NamespaceString> nss;
    std::vector<ClusterClientCursorParams::RemoteCursor> remotes;
    for (auto&& cursor : serializedRemotes) {
        BSONElement nsElem;
        BSONElement hostElem;
        BSONElement idElem;
        uassert(17027,
                "$mergeCursors stage requires each cursor in array to be an object",
                cursor.type() == BSONType::Object);
        for (auto&& cursorElem : cursor.Obj()) {
            const auto fieldName = cursorElem.fieldNameStringData();
            if (fieldName == "ns"_sd) {
                nsElem = cursorElem;
            } else if (fieldName == "host"_sd) {
                hostElem = cursorElem;
            } else if (fieldName == "id"_sd) {
                idElem = cursorElem;
            } else {
                uasserted(50730,
                          str::stream() << "Unrecognized option " << fieldName
                                        << " within cursor provided to $mergeCursors: "
                                        << cursor);
            }
        }
        uassert(
            50731,
            "$mergeCursors stage requires \'ns\' field with type string for each cursor in array",
            nsElem.type() == BSONType::String);

        // We require each cursor to have the same namespace. This isn't a fundamental limit of the
        // system, but needs to be true due to the implementation of AsyncResultsMerger, which
        // tracks one namespace for all cursors.
        uassert(50720,
                "$mergeCursors requires each cursor to have the same namespace",
                !nss || nss->ns() == nsElem.valueStringData());
        nss = NamespaceString(nsElem.String());

        uassert(
            50721,
            "$mergeCursors stage requires \'host\' field with type string for each cursor in array",
            hostElem.type() == BSONType::String);
        auto host = uassertStatusOK(HostAndPort::parse(hostElem.valueStringData()));

        uassert(50722,
                "$mergeCursors stage requires \'id\' field with type long for each cursor in array",
                idElem.type() == BSONType::NumberLong);
        auto cursorId = idElem.Long();

        // We are assuming that none of the cursors have been iterated at all, and so will not have
        // any data in the initial batch.
        // TODO SERVER-33323 We use a fake shard id because the AsyncResultsMerger won't use it for
        // anything, and finding the real one is non-trivial.
        std::vector<BSONObj> emptyBatch;
        remotes.push_back({ShardId("fakeShardIdForMergeCursors"),
                           std::move(host),
                           CursorResponse{*nss, cursorId, emptyBatch}});
    }
    invariant(nss);  // We know there is at least one cursor in 'serializedRemotes', and we require
                     // each cursor to have a 'ns' field.

    auto params = stdx::make_unique<ClusterClientCursorParams>(*nss);
    params->remotes = std::move(remotes);
    return new DocumentSourceMergeCursors(
        Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
        std::move(params),
        expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::create(
    std::vector<ClusterClientCursorParams::RemoteCursor>&& remoteCursors,
    executor::TaskExecutor* executor,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto params = stdx::make_unique<ClusterClientCursorParams>(expCtx->ns);
    params->remotes = std::move(remoteCursors);
    return new DocumentSourceMergeCursors(executor, std::move(params), expCtx);
}

void DocumentSourceMergeCursors::detachFromOperationContext() {
    if (_arm) {
        _arm->detachFromOperationContext();
    }
}

void DocumentSourceMergeCursors::reattachToOperationContext(OperationContext* opCtx) {
    if (_arm) {
        _arm->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceMergeCursors::doDispose() {
    if (_arm) {
        _arm->blockingKill(pExpCtx->opCtx);
    }
}

}  // namespace mongo
