/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

// No privileges are required to run this agg stage as it is only available with enableTestCommands.
REGISTER_TEST_DOCUMENT_SOURCE(listMqlEntities,
                              DocumentSourceListMqlEntities::LiteParsed::parse,
                              DocumentSourceListMqlEntities::createFromBson);
ALLOCATE_DOCUMENT_SOURCE_ID(listMqlEntities, DocumentSourceListMqlEntities::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListMqlEntities::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    invariant(
        stageName == kStageName,
        str::stream() << "Unexpected stage registered with DocumentSourceListMqlEntities parser: "
                      << stageName);
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
                                        HostTypeRequirement::kLocalOnly,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
                                        UnionRequirement::kNotAllowed};
    constraints.isIndependentOfAnyCollection = true;
    constraints.setConstraintsForNoInputSources();
    return constraints;
}

const char* DocumentSourceListMqlEntities::getSourceName() const {
    return kStageName.data();
}

DocumentSourceContainer::iterator DocumentSourceListMqlEntities::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    return std::next(itr);
}

Value DocumentSourceListMqlEntities::serialize(const SerializationOptions& opts) const {
    return Value(DOC(kStageName << DOC(kEntityTypeFieldName << MqlEntityType_serializer(_type))));
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceListMqlEntities::distributedPlanLogic() {
    return boost::none;
}

}  // namespace mongo
