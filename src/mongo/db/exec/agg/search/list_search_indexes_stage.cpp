/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/agg/search/list_search_indexes_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/query/search/search_index_common.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceListSearchIndexesToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceListSearchIndexes*>(source.get());

    tassert(10807802, "expected 'DocumentSourceListSearchIndexes' type", documentSource);

    return make_intrusive<exec::agg::ListSearchIndexesStage>(
        documentSource->kStageName, documentSource->getExpCtx(), documentSource->_cmdObj);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(listSearchIndexesStage,
                           DocumentSourceListSearchIndexes::id,
                           documentSourceListSearchIndexesToStageFn);

ListSearchIndexesStage::ListSearchIndexesStage(
    StringData stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx, BSONObj cmdObj)
    : Stage(stageName, pExpCtx), _cmdObj(cmdObj.getOwned()) {}

GetNextResult ListSearchIndexesStage::doGetNext() {
    auto* opCtx = pExpCtx->getOperationContext();
    // Cache the collectionUUID for subsequent 'doGetNext' calls on _searchIndexCommandInfo struct.
    // We cannot use 'pExpCtx->getUUID()' like other aggregation stages, because this stage can run
    // directly on mongos. 'pExpCtx->getUUID()' will always be null on mongos.
    if (!_collectionUUID) {
        auto retrieveCollectionUUIDAndResolveViewStatus = retrieveCollectionUUIDAndResolveView(
            opCtx, pExpCtx->getUserNss(), /*failOnTsColl=*/false);

        // Ignore any error sent by retrieveCollectionUUIDAndResolveView. This issue will be handled
        // below by returning EOF.
        if (retrieveCollectionUUIDAndResolveViewStatus.isOK()) {
            std::tie(_collectionUUID, _resolvedNamespace, _view) =
                retrieveCollectionUUIDAndResolveViewStatus.getValue();
        }
    }

    // Return EOF if the collection requested does not exist.
    if (!_collectionUUID || _eof) {
        return GetNextResult::makeEOF();
    }

    /**
     * The user command field of the 'manageSearchIndex' command should be the stage issued by the
     * user. The 'id' field and 'name' field are optional, so possible user commands can be:
     * $listSearchIndexes: {}
     * $listSearchIndexes: { id: "<index id>" }
     * $listSearchIndexes: { name: "<index name>"}
     */
    if (_searchIndexes.empty()) {
        BSONObjBuilder bob;
        bob.append(DocumentSourceListSearchIndexes::kStageName, _cmdObj);
        auto cmdBson = bob.done();
        // Sends a manageSearchIndex command and returns a cursor with index information.
        BSONObj manageSearchIndexResponse =
            runSearchIndexCommand(opCtx, *_resolvedNamespace, cmdBson, *_collectionUUID, _view);
        /**
         * 'mangeSearchIndex' returns a cursor with the following fields:
         * cursor: {
         *   id: Long("0"),
         *   ns: "<database name>.
         *   firstBatch: [ // There will only ever be one batch.
         *       {<document>},
         *       {<document>} ],
         * }
         * We need to return the documents in the 'firstBatch' field.
         */
        auto cursor =
            manageSearchIndexResponse.getField(DocumentSourceListSearchIndexes::kCursorFieldName);
        tassert(
            7486302,
            "The internal command manageSearchIndex should return a 'cursor' field with an object.",
            !cursor.eoo() && cursor.type() == BSONType::object);

        cursor = cursor.Obj().getField(DocumentSourceListSearchIndexes::kFirstBatchFieldName);
        tassert(7486303,
                "The internal command manageSearchIndex should return an array in the 'firstBatch' "
                "field",
                !cursor.eoo() && cursor.type() == BSONType::array);
        auto searchIndexes = cursor.Array();

        // If the manageSearchIndex command didn't return any documents, we should return EOF.
        if (searchIndexes.empty()) {
            _eof = true;
            return GetNextResult::makeEOF();
        }
        // We have to convert all the BSONElement to owned BSONObj, so they are valid across
        // getMore calls.
        for (BSONElement e : searchIndexes) {
            tassert(
                7486304,
                str::stream() << "The internal command manageSearchIndex should return documents "
                                 "inside the 'firstBatch' field but found a bad entry: "
                              << (e.eoo() ? "EOO" : e.toString()),
                e.type() == BSONType::object);
            _searchIndexes.push(e.Obj().getOwned());
        }
    }

    Document doc{std::move(_searchIndexes.front())};
    _searchIndexes.pop();
    // Check if we should return EOF in the next 'getMore' call.
    if (_searchIndexes.empty()) {
        _eof = true;
    }
    return doc;
}
}  // namespace exec::agg
}  // namespace mongo
