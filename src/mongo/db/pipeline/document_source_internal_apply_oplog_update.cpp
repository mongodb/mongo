/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_apply_oplog_update.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/document_source_internal_apply_oplog_update_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/update/update_driver.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalApplyOplogUpdate,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalApplyOplogUpdate::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalApplyOplogUpdate::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6315901,
            str::stream() << "Argument to " << kStageName
                          << " stage must be an object, but found type: " << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec =
        InternalApplyOplogUpdateSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    return new DocumentSourceInternalApplyOplogUpdate(pExpCtx, spec.getOplogUpdate());
}

DocumentSourceInternalApplyOplogUpdate::DocumentSourceInternalApplyOplogUpdate(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const BSONObj& oplogUpdate)
    : DocumentSource(kStageName, pExpCtx),
      _oplogUpdate(std::move(oplogUpdate)),
      _updateDriver(pExpCtx) {
    // Parse the raw oplog update description.
    const auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        _oplogUpdate, {true /* mustCheckExistenceForInsertOperations */});

    // UpdateDriver only expects to apply a diff in the context of oplog application.
    _updateDriver.setFromOplogApplication(true);
    _updateDriver.parse(updateMod, {});
}

DocumentSource::GetNextResult DocumentSourceInternalApplyOplogUpdate::doGetNext() {
    auto next = pSource->getNext();
    if (!next.isAdvanced()) {
        return next;
    }

    // Use _updateDriver to apply the update to the document.
    mutablebson::Document doc(next.getDocument().toBson());
    uassertStatusOK(_updateDriver.update(pExpCtx->opCtx,
                                         StringData(),
                                         &doc,
                                         false /* validateForStorage */,
                                         FieldRefSet(),
                                         false /* isInsert */));

    return Document(doc.getObject());
}

Value DocumentSourceInternalApplyOplogUpdate::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{kStageName, Document{{kOplogUpdateFieldName, _oplogUpdate}}}});
}

}  // namespace mongo
