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

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(listExtensions,
                                           DocumentSourceListExtensions::LiteParsed::parse,
                                           DocumentSourceListExtensions::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagExtensionsAPI);

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

    // Get the loaded extensions and insert them in the queue with the format {"extensionName":
    // "..."}. The queue won't be populated until the first call to getNext().
    DocumentSourceQueue::DeferredQueue deferredQueue{[]() {
        const auto loadedExtensions = extension::host::ExtensionLoader::getLoadedExtensions();
        std::deque<DocumentSource::GetNextResult> queue;

        for (const auto& path : loadedExtensions) {
            // TODO(SERVER-110317): Remove path truncation.
            const auto extensionName = std::filesystem::path(path).stem().string();
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
