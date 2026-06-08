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

#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalAssertDataAssumptions,
                                     InternalAssertDataAssumptionsLiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalAssertDataAssumptions,
                                                   DocumentSourceInternalAssertDataAssumptions,
                                                   InternalAssertDataAssumptionsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalAssertDataAssumptions,
                            DocumentSourceInternalAssertDataAssumptions::id);

constexpr StringData DocumentSourceInternalAssertDataAssumptions::kStageName;

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalAssertDataAssumptions::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    uassert(9587700,
            str::stream() << kStageName << " must be specified as an object, got: " << elem,
            elem.type() == BSONType::object);

    BSONObj spec = elem.Obj();
    std::set<FieldPath> nonArrayPaths;

    // Parse the specification: expect an array of field paths in "paths" field
    if (auto pathsElem = spec["paths"]; !pathsElem.eoo()) {
        uassert(9587701,
                str::stream() << kStageName
                              << " 'paths' field must be an array, got: " << pathsElem,
                pathsElem.type() == BSONType::array);

        for (const auto& pathElem : pathsElem.Obj()) {
            uassert(9587702,
                    str::stream() << kStageName
                                  << " 'paths' array elements must be strings, got: " << pathElem,
                    pathElem.type() == BSONType::string);
            nonArrayPaths.insert(FieldPath(pathElem.String()));
        }
    }

    return create(expCtx, std::move(nonArrayPaths));
}

boost::intrusive_ptr<DocumentSourceInternalAssertDataAssumptions>
DocumentSourceInternalAssertDataAssumptions::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths) {
    return make_intrusive<DocumentSourceInternalAssertDataAssumptions>(expCtx,
                                                                       std::move(nonArrayPaths));
}

DepsTracker::State DocumentSourceInternalAssertDataAssumptions::getDependencies(
    DepsTracker* deps) const {
    return DepsTracker::State::SEE_NEXT;
}

DocumentSourceInternalAssertDataAssumptions::DocumentSourceInternalAssertDataAssumptions(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::set<FieldPath> nonArrayPaths)
    : DocumentSource(kStageName, expCtx), _nonArrayPaths(std::move(nonArrayPaths)) {}


Value DocumentSourceInternalAssertDataAssumptions::serialize(
    const SerializationOptions& opts) const {
    MutableDocument spec;
    std::vector<Value> paths;
    paths.reserve(_nonArrayPaths.size());
    for (const auto& path : _nonArrayPaths) {
        paths.push_back(Value(opts.serializeFieldPath(path)));
    }
    spec["paths"] = Value(std::move(paths));
    return Value(DOC(getSourceName() << spec.freeze()));
}

}  // namespace mongo
