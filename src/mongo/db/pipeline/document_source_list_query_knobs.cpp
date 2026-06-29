/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

    std::deque<DocumentSource::GetNextResult> queue;
    for (const auto& entry : QueryKnobRegistry::instance().entries()) {
        BSONObjBuilder bob;
        bob.append("name", entry.param->name());
        bob.append("wireName", entry.wireName);
        bob.append("pqsSettable", entry.pqsSettable);
        entry.appendType(&bob);
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
