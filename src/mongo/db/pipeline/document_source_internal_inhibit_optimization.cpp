/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalInhibitOptimization,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalInhibitOptimization::createFromBson);

constexpr StringData DocumentSourceInternalInhibitOptimization::kStageName;

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalInhibitOptimization::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "$_internalInhibitOptimization must take a nested object but found: "
                          << elem,
            elem.type() == BSONType::Object);

    auto specObj = elem.embeddedObject();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$_internalInhibitOptimization must take an empty object but found: "
                          << specObj,
            specObj.isEmpty());

    return new DocumentSourceInternalInhibitOptimization(expCtx);
}

DocumentSource::GetNextResult DocumentSourceInternalInhibitOptimization::getNext() {
    pExpCtx->checkForInterrupt();
    return pSource->getNext();
}

Value DocumentSourceInternalInhibitOptimization::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{getSourceName(), Value{Document{}}}});
}

}  // namesace mongo
