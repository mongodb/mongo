// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_list_mql_entities.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

// No privileges are required to run this agg stage as it is only available with enableTestCommands.
REGISTER_TEST_LITE_PARSED_DOCUMENT_SOURCE(listMqlEntities,
                                          DocumentSourceListMqlEntities::LiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listMqlEntities,
                                                   DocumentSourceListMqlEntities,
                                                   ListMqlEntitiesStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(listMqlEntities, DocumentSourceListMqlEntities::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListMqlEntities::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    tassert(
        11282984,
        str::stream() << "Unexpected stage registered with DocumentSourceListMqlEntities parser: "
                      << stageName,
        stageName == kStageName);
    uassert(9590101,
            str::stream() << "expected an object as specification for " << kStageName
                          << " stage, got " << typeName(elem.type()),
            elem.type() == BSONType::object);
    const auto& nss = expCtx->getNamespaceString();
    uassert(ErrorCodes::InvalidNamespace,
            "$listMqlEntities must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());
    auto spec = ListMqlEntitiesSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    return new DocumentSourceListMqlEntities(expCtx, spec.getEntityType());
}

DocumentSourceListMqlEntities::DocumentSourceListMqlEntities(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, MqlEntityTypeEnum type)
    : DocumentSource(kStageName, expCtx), _type(type) {
    if (_type != MqlEntityTypeEnum::aggregationStages) {
        MONGO_UNIMPLEMENTED;
    }
}

StageConstraints DocumentSourceListMqlEntities::constraints(PipelineSplitState pipeState) const {
    auto constraints = StageConstraints{StreamType::kStreaming,
                                        PositionRequirement::kFirst,
                                        HostTypeRequirement::kReceivingHostOnly,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
                                        UnionRequirement::kNotAllowed};
    constraints.isIndependentOfAnyCollection = true;
    constraints.setConstraintsForNoInputSources();
    return constraints;
}

std::string_view DocumentSourceListMqlEntities::getSourceName() const {
    return kStageName;
}

DocumentSourceContainer::iterator DocumentSourceListMqlEntities::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    return std::next(itr);
}

Value DocumentSourceListMqlEntities::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(DOC(kStageName << DOC(kEntityTypeFieldName << idl::serialize(_type))));
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceListMqlEntities::distributedPlanLogic(const DistributedPlanContext* ctx) {
    return boost::none;
}

}  // namespace mongo
