// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_query_knobs.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {


REGISTER_TEST_LITE_PARSED_DOCUMENT_SOURCE(listQueryKnobs,
                                          DocumentSourceListQueryKnobs::LiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listQueryKnobs,
                                                   DocumentSourceListQueryKnobs,
                                                   ListQueryKnobsStageParams);

boost::intrusive_ptr<DocumentSource> DocumentSourceListQueryKnobs::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    tassert(10491900,
            str::stream() << "Unexpected stage registered with "
                             "DocumentSourceListQueryKnobs parser: "
                          << stageName,
            stageName == kStageName);
    uassert(10491901,
            str::stream() << "expected an object as specification for " << kStageName
                          << " stage, got " << typeName(elem.type()),
            elem.type() == BSONType::object);
    uassert(10491902,
            str::stream() << "expected an empty object as specification for " << kStageName
                          << " stage, got " << elem,
            elem.Obj().isEmpty());
    uassert(ErrorCodes::InvalidNamespace,
            "$listQueryKnobs must be run against the 'admin' database with {aggregate: 1}",
            expCtx->getNamespaceString().isAdminDB() &&
                expCtx->getNamespaceString().isCollectionlessAggregateNS());

    const auto defaults = QueryKnobSnapshotCache::instance().getDefaults();
    std::deque<DocumentSource::GetNextResult> queue;
    for (const auto& entry : QueryKnobRegistry::instance().entries()) {
        BSONObjBuilder bob;
        bob.append("name", entry.param->name());
        bob.append("wireName", entry.wireName);
        bob.append("pqsSettable", entry.pqsSettable);
        entry.appendType(&bob);
        entry.appendConstraints(&bob);
        auto defaultValue = defaults.getValue(entry.id);
        entry.toBSON(bob, "default", defaultValue);
        queue.push_back(Document(bob.obj()));
    }

    return make_intrusive<DocumentSourceQueue>(
        std::move(queue),
        expCtx,
        /* stageNameOverride */ kStageName,
        /* serializeOverride */ Value(DOC(kStageName << Document())),
        /* constraintsOverride */ constraints());
}

}  // namespace mongo
