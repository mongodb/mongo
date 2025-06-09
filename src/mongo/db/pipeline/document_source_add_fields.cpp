/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_add_fields.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(addFields,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceAddFields::createFromBson,
                         AllowedWithApiStrict::kAlways);
REGISTER_DOCUMENT_SOURCE(set,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceAddFields::createFromBson,
                         AllowedWithApiStrict::kAlways);

intrusive_ptr<DocumentSource> DocumentSourceAddFields::create(
    BSONObj addFieldsSpec,
    const intrusive_ptr<ExpressionContext>& expCtx,
    StringData userSpecifiedName) {

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
    invariant(specifiedName == kStageName || specifiedName == kAliasNameSet);

    uassert(40272,
            str::stream() << specifiedName << " specification stage must be an object, got "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    return DocumentSourceAddFields::create(elem.Obj(), expCtx, specifiedName);
}
}  // namespace mongo
