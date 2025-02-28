/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"

#include "mongo/db/query/search/search_index_command_testing_helper.h"
#include "mongo/db/query/search/search_index_common.h"
#include "mongo/db/query/search/search_index_process_interface.h"
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
// Whereas create/update/drop search index commands throw if the collection doesn't exist,
// $listSearchIndexes just returns 0 documents.
std::tuple<boost::optional<UUID>,
           const NamespaceString,
           boost::optional<NamespaceString>,
           boost::optional<std::vector<BSONObj>>>
retrieveCollectionUUIDAndResolveView(const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    auto* opCtx = pExpCtx->getOperationContext();
    // TLDR: on mongod, pass the viewName saved to expCtx. On mongos, pass the NSS saved to
    // expCtx.
    // On mongod, pExpCtx->getNamespaceString() represents the resolved namespace (the
    // underlying namespace). To correctly identify if the
    // query is being run on a view, we need to pass the nss the user is running their original
    // query against. However, on mongod, pExpCtx->getNamespaceString() already represents the
    // resolved nss (eg the underlying source collection nss). Thus if we are on mongod, we
    // should pass the view NS saved to the expression context. On mongos, $listSearchIndexes
    // queries are never resolved thus there is no view NS on the expression context. Luckily,
    // because these queries are never resolved, pExpCtx->getNamespaceString() still represents
    // the view nss.
    auto currentOperationNss =
        pExpCtx->getViewNSForMongotIndexedView().value_or(pExpCtx->getNamespaceString());
    auto collUUIDResolvedViewPair =
        SearchIndexProcessInterface::get(opCtx)->fetchCollectionUUIDAndResolveView(
            opCtx, currentOperationNss);
    // If the query is on a normal collection, the source collection will be the same as
    // the current NS.
    auto sourceCollectionNss = currentOperationNss;
    boost::optional<NamespaceString> viewNss;
    boost::optional<std::vector<BSONObj>> viewPipeline;
    boost::optional<UUID> collUUID = collUUIDResolvedViewPair.first;
    if (auto resolvedView = collUUIDResolvedViewPair.second) {
        uassert(
            ErrorCodes::QueryFeatureNotAllowed,
            "search index commands on views are not allowed in the current configuration. "
            "You may need to enable the "
            "correponding feature flag",
            feature_flags::gFeatureFlagMongotIndexedViews.isEnabledUseLatestFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
        // The request is on a view! Therefore, currentOperationNss refers to the view
        // NS and the namespace on resolvedView refers to the underlying source collection.
        sourceCollectionNss = resolvedView.value().getNamespace();
        viewNss.emplace(currentOperationNss);
        viewPipeline.emplace(resolvedView.value().getPipeline());
    }

    return std::make_tuple(collUUID, sourceCollectionNss, viewNss, viewPipeline);
}
}  // namespace


REGISTER_DOCUMENT_SOURCE(listSearchIndexes,
                         DocumentSourceListSearchIndexes::LiteParsedListSearchIndexes::parse,
                         DocumentSourceListSearchIndexes::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1)
ALLOCATE_DOCUMENT_SOURCE_ID(listSearchIndexes, DocumentSourceListSearchIndexes::id)

void DocumentSourceListSearchIndexes::validateListSearchIndexesSpec(
    const DocumentSourceListSearchIndexesSpec* spec) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot set both 'name' and 'id' for $listSearchIndexes.",
            !(spec->getId() && spec->getName()));
};

boost::intrusive_ptr<DocumentSource> DocumentSourceListSearchIndexes::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // We must validate if atlas is configured. However, we might just be parsing or validating the
    // query without executing it. In this scenario, there is no reason to check if we are running
    // with atlas configured, since we will never make a call to the search index management host.
    // For example, if we are in query analysis, performing pipeline-style updates, or creating
    // query shapes. Additionally, it would be an error to validate this inside query analysis,
    // since query analysis doesn't have access to the search index management host.
    //
    // This validation should occur before parsing so in the case of a parse and configuration
    // error, the configuration error is thrown.
    if (pExpCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
        throwIfNotRunningWithRemoteSearchIndexManagement();
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The $listSearchIndexes stage specification must be an object. Found: "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);
    auto spec = DocumentSourceListSearchIndexesSpec::parse(IDLParserContext(kStageName),
                                                           elem.embeddedObject());

    return new DocumentSourceListSearchIndexes(pExpCtx, elem.Obj());
}

Value DocumentSourceListSearchIndexes::serialize(const SerializationOptions& opts) const {
    BSONObjBuilder bob;
    auto spec = DocumentSourceListSearchIndexesSpec::parse(IDLParserContext(kStageName), _cmdObj);
    spec.serialize(&bob, opts);
    return Value(Document{{kStageName, bob.done()}});
}

// We use 'kLocalOnly' because the aggregation request can be handled by a shard or mongos depending
// on where the user sends the request.
StageConstraints DocumentSourceListSearchIndexes::constraints(
    Pipeline::SplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kLocalOnly,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed,
                                 ChangeStreamRequirement::kDenylist);
    constraints.requiresInputDocSource = false;
    return constraints;
}

DocumentSource::GetNextResult DocumentSourceListSearchIndexes::doGetNext() {
    auto* opCtx = pExpCtx->getOperationContext();
    // Cache the collectionUUID for subsequent 'doGetNext' calls on _searchIndexCommandInfo struct.
    // We cannot use 'pExpCtx->getUUID()' like other aggregation stages, because this stage can run
    // directly on mongos. 'pExpCtx->getUUID()' will always be null on mongos.
    if (!_collectionUUID) {
        std::tie(_collectionUUID, _resolvedNamespace, _viewName, _viewPipeline) =
            retrieveCollectionUUIDAndResolveView(pExpCtx);
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
        bob.append(kStageName, _cmdObj);
        auto cmdBson = bob.done();
        // Sends a manageSearchIndex command and returns a cursor with index information.
        BSONObj manageSearchIndexResponse =
            runSearchIndexCommand(opCtx, *_resolvedNamespace, cmdBson, *_collectionUUID, _viewName);
        search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting(
            opCtx, *_resolvedNamespace, cmdBson, _viewName, _viewPipeline);
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
        auto cursor = manageSearchIndexResponse.getField(kCursorFieldName);
        tassert(
            7486302,
            "The internal command manageSearchIndex should return a 'cursor' field with an object.",
            !cursor.eoo() && cursor.type() == BSONType::Object);

        cursor = cursor.Obj().getField(kFirstBatchFieldName);
        tassert(7486303,
                "The internal command manageSearchIndex should return an array in the 'firstBatch' "
                "field",
                !cursor.eoo() && cursor.type() == BSONType::Array);
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
                e.type() == BSONType::Object);
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

}  // namespace mongo
