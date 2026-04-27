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

#include "mongo/db/pipeline/lite_parsed_graph_lookup.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_graph_lookup_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(graphLookup,
                                     LiteParsedGraphLookUp::parse,
                                     AllowedWithApiStrict::kAlways);

namespace {

NamespaceString parseFromAndResolveNamespace(const BSONElement& elem,
                                             const DatabaseName& defaultDb) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$graphLookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::string);
    NamespaceString fromNss(NamespaceStringUtil::deserialize(defaultDb, elem.valueStringData()));
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $graphLookup namespace: " << fromNss.toStringForErrorMsg(),
            fromNss.isValid());
    return fromNss;
}

/**
 * Extracts a string-typed field from the parsed IDL spec. 'elem' must be present (empty BSONElement
 * is not accepted).
 */
std::string requireString(StringData fieldName, BSONElement elem) {
    uassert(12109311,
            str::stream() << "$graphLookup '" << fieldName << "' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::string);
    return elem.String();
}

/**
 * Validates maxDepth and returns the validated long long.
 */
long long validateMaxDepth(BSONElement elem) {
    uassert(12109307,
            str::stream() << "$graphLookup 'maxDepth' must be numeric, but found type: "
                          << typeName(elem.type()),
            elem.isNumber());
    long long value = elem.safeNumberLong();
    uassert(12109308,
            str::stream() << "$graphLookup 'maxDepth' must be nonnegative, but found: " << value,
            value >= 0);
    uassert(12109309,
            str::stream() << "$graphLookup 'maxDepth' must be an integer representable as a long "
                             "long, but found: "
                          << elem.number(),
            static_cast<double>(value) == elem.number());
    return value;
}

}  // namespace

LiteParsedGraphLookUp::LiteParsedGraphLookUp(const BSONElement& spec,
                                             NamespaceString foreignNss,
                                             boost::optional<FieldPath> as,
                                             boost::optional<FieldPath> connectFromField,
                                             boost::optional<FieldPath> connectToField,
                                             boost::optional<BSONElement> startWith,
                                             boost::optional<BSONObj> additionalFilter,
                                             boost::optional<FieldPath> depthField,
                                             boost::optional<long long> maxDepth)
    // TODO SERVER-125119 $graphLookup does not yet accept a user-provided sub-pipeline, but it
    // is modeled as a LiteParsedDocumentSourceNestedPipelines so that view definitions resolved
    // from the foreign namespace can be attached as sub-pipelines during lite parsing.
    : LiteParsedDocumentSourceNestedPipelines(
          spec, std::move(foreignNss), std::vector<LiteParsedPipeline>{}),
      _as(std::move(as)),
      _connectFromField(std::move(connectFromField)),
      _connectToField(std::move(connectToField)),
      _startWith(startWith),
      _additionalFilter(std::move(additionalFilter)),
      _depthField(std::move(depthField)),
      _maxDepth(maxDepth) {}

std::unique_ptr<LiteParsedGraphLookUp> LiteParsedGraphLookUp::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $graphLookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    DocumentSourceGraphLookUpSpec parsedSpec;
    try {
        parsedSpec = DocumentSourceGraphLookUpSpec::parse(spec.embeddedObject(),
                                                          IDLParserContext(kStageName));
    } catch (const ExceptionFor<ErrorCodes::IDLUnknownField>& ex) {
        uasserted(12109312, str::stream() << "unknown argument to $graphLookup: " << ex.reason());
    }

    const auto fromAny = parsedSpec.getFrom();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $graphLookup stage specification: "
                          << spec.embeddedObject(),
            fromAny.has_value());
    auto foreignNss = parseFromAndResolveNamespace(fromAny->getElement(), nss.dbName());

    boost::optional<FieldPath> as;
    if (const auto& a = parsedSpec.getAs()) {
        as = FieldPath{requireString("as", a->getElement())};
    }
    boost::optional<FieldPath> connectFromField;
    if (const auto& cff = parsedSpec.getConnectFromField()) {
        connectFromField = FieldPath{requireString("connectFromField", cff->getElement())};
    }
    boost::optional<FieldPath> connectToField;
    if (const auto& ctf = parsedSpec.getConnectToField()) {
        connectToField = FieldPath{requireString("connectToField", ctf->getElement())};
    }

    boost::optional<FieldPath> depthField;
    if (const auto& df = parsedSpec.getDepthField()) {
        depthField = FieldPath{requireString("depthField", df->getElement())};
    }

    boost::optional<long long> maxDepth;
    if (const auto& md = parsedSpec.getMaxDepth()) {
        maxDepth = validateMaxDepth(md->getElement());
    }

    boost::optional<BSONObj> additionalFilter;
    if (const auto& filter = parsedSpec.getAdditionalFilter()) {
        auto elem = filter->getElement();
        uassert(12109310,
                str::stream() << "$graphLookup 'restrictSearchWithMatch' must be an object, but "
                                 "found "
                              << typeName(elem.type()),
                elem.type() == BSONType::object);
        additionalFilter = elem.embeddedObject().getOwned();
    }

    boost::optional<BSONElement> startWith;
    if (const auto& sw = parsedSpec.getStartWith()) {
        startWith = sw->getElement();
    }

    return std::make_unique<LiteParsedGraphLookUp>(spec,
                                                   std::move(foreignNss),
                                                   std::move(as),
                                                   std::move(connectFromField),
                                                   std::move(connectToField),
                                                   startWith,
                                                   std::move(additionalFilter),
                                                   std::move(depthField),
                                                   maxDepth);
}

PrivilegeVector LiteParsedGraphLookUp::requiredPrivileges(bool isMongos,
                                                          bool bypassDocumentValidation) const {
    // TODO SERVER-125119 Once $graphLookup populates `_pipelines` (e.g. via view definition
    // resolution), this must also account for the privileges required by each pipeline in
    // `_pipelines` in addition to `_foreignNss`.
    tassert(12509600, "Expected foreign namespace to be set for $graphLookup", _foreignNss);
    return {Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find)};
}

Status LiteParsedGraphLookUp::checkShardedForeignCollAllowed(
    const NamespaceString& nss, bool inMultiDocumentTransaction) const {
    tassert(12509601, "Expected foreign namespace to be set for $graphLookup", _foreignNss);
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    if (!inMultiDocumentTransaction || *_foreignNss != nss ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot)) {
        return Status::OK();
    }
    return Status(ErrorCodes::NamespaceCannotBeSharded,
                  "Sharded $graphLookup is not allowed within a multi-document transaction");
}

std::unique_ptr<StageParams> LiteParsedGraphLookUp::getStageParams() const {
    return std::make_unique<GraphLookUpStageParams>(*_foreignNss,
                                                    _as,
                                                    _connectFromField,
                                                    _connectToField,
                                                    _startWith,
                                                    _additionalFilter,
                                                    _depthField,
                                                    _maxDepth,
                                                    getOriginalBson());
}

}  // namespace mongo
