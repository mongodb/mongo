// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_add_fields.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(addFields,
                                     AddFieldsLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);
REGISTER_LITE_PARSED_DOCUMENT_SOURCE(set,
                                     AddFieldsLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(addFields,
                                                   DocumentSourceAddFields,
                                                   AddFieldsStageParams);

intrusive_ptr<DocumentSource> DocumentSourceAddFields::create(
    BSONObj addFieldsSpec,
    const intrusive_ptr<ExpressionContext>& expCtx,
    std::string_view userSpecifiedName) {

    const bool isIndependentOfAnyCollection = false;
    intrusive_ptr<DocumentSourceSingleDocumentTransformation> addFields(
        new DocumentSourceSingleDocumentTransformation(
            expCtx,
            [&]() {
                try {
                    return projection_executor::AddFieldsProjectionExecutor::create(expCtx,
                                                                                    addFieldsSpec);
                } catch (DBException& ex) {
                    ex.addContext("Invalid " + std::string{userSpecifiedName});
                    throw;
                }
            }(),
            userSpecifiedName == kStageName ? kStageName : kAliasNameSet,
            isIndependentOfAnyCollection));
    return addFields;
}

intrusive_ptr<DocumentSource> DocumentSourceAddFields::create(
    const FieldPath& fieldPath,
    const intrusive_ptr<Expression>& expr,
    const intrusive_ptr<ExpressionContext>& expCtx) {

    const bool isIndependentOfAnyCollection = false;
    auto docSrc = make_intrusive<DocumentSourceSingleDocumentTransformation>(
        expCtx,
        projection_executor::AddFieldsProjectionExecutor::create(expCtx, fieldPath, expr),
        kStageName,
        isIndependentOfAnyCollection);
    return docSrc;
}

intrusive_ptr<DocumentSource> DocumentSourceAddFields::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    const auto specifiedName = elem.fieldNameStringData();
    tassert(11294811,
            str::stream() << "Attempting to parse DocumentSourceAddFields from neither "
                          << kStageName << ", nor " << kAliasNameSet << " BSON object",
            specifiedName == kStageName || specifiedName == kAliasNameSet);

    uassert(40272,
            str::stream() << specifiedName << " specification stage must be an object, got "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    return DocumentSourceAddFields::create(elem.Obj(), expCtx, specifiedName);
}
}  // namespace mongo
