// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_extensions.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <functional>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(
    listExtensions,
    DocumentSourceListExtensions::LiteParsed::parse,
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagExtensionsAPI);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listExtensions,
                                                   DocumentSourceListExtensions,
                                                   ListExtensionsStageParams);

// Implements DocumentSourceListExtensions based on DocumentSourceQueue stage.
boost::intrusive_ptr<DocumentSource> DocumentSourceListExtensions::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    tassert(
        10983500,
        str::stream() << "Unexpected stage registered with DocumentSourceListExtensions parser: "
                      << stageName,
        stageName == kStageName);
    uassert(10983501,
            str::stream() << "expected an object as specification for " << kStageName
                          << " stage, got " << typeName(elem.type()),
            elem.type() == BSONType::object);
    uassert(10983502,
            str::stream() << "expected an empty object as specification for " << kStageName
                          << " stage, got " << elem,
            elem.Obj().isEmpty());

    const auto& nss = expCtx->getNamespaceString();
    uassert(ErrorCodes::InvalidNamespace,
            "$listExtensions must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());

    // Get the loaded extensions and insert them in the queue with the format
    // {"extensionName": "..."}. The queue won't be populated until the first call to getNext().
    DocumentSourceQueue::DeferredQueue deferredQueue{[]() {
        const auto loadedExtensions = extension::host::ExtensionLoader::getLoadedExtensions();
        std::deque<DocumentSource::GetNextResult> queue;

        for (const auto& [extensionName, _] : loadedExtensions) {
            queue.push_back(Document(BSON("extensionName" << extensionName)));
        }

        // Canonicalize output order of results. Sort in ascending order so that
        // QueueStage can use pop_front() to return the results in order.
        std::sort(queue.begin(), queue.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.getDocument()["extensionName"].getString() <
                rhs.getDocument()["extensionName"].getString();
        });

        return queue;
    }};

    return make_intrusive<DocumentSourceQueue>(
        std::move(deferredQueue),
        expCtx,
        /* stageNameOverride */ kStageName,
        /* serializeOverride*/ Value(DOC(kStageName << Document())),
        /* constraintsOverride */ constraints());
}

}  // namespace mongo
