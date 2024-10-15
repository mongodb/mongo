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

#include "mongo/db/pipeline/document_source_internal_replace_root.h"

namespace mongo {

const char* DocumentSourceInternalReplaceRoot::getSourceName() const {
    return kStageNameInternal.rawData();
}

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalReplaceRoot,
                                  LiteParsedDocumentSourceInternal::parse,
                                  DocumentSourceInternalReplaceRoot::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalReplaceRoot::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(8105802,
            str::stream() << "$_internalReplaceRoot expects a sub-document but found: " << elem,
            elem.type() == BSONType::Object);
    return nullptr;
}

Pipeline::SourceContainer::iterator DocumentSourceInternalReplaceRoot::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);
    return itr;
}

Value DocumentSourceInternalReplaceRoot::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _newRoot->serialize()}});
}

DocumentSource::GetNextResult DocumentSourceInternalReplaceRoot::doGetNext() {
    tasserted(8105803, "Execution reached non-executable pipeline stage");
}
}  // namespace mongo
