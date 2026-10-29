// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_graph_lookup.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_graph_lookup_gen.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

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
std::string requireString(std::string_view fieldName, BSONElement elem) {
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
                                             boost::optional<BSONObj> startWith,
                                             boost::optional<BSONObj> additionalFilter,
                                             boost::optional<FieldPath> depthField,
                                             boost::optional<long long> maxDepth,
                                             const LiteParserOptions& options,
                                             boost::optional<OwnedLiteParsedPipeline> fromPipeline)
    : LiteParsedDocumentSourceNestedPipelines(
          spec, options, std::move(foreignNss), std::vector<OwnedLiteParsedPipeline>{}),
      _as(std::move(as)),
      _connectFromField(std::move(connectFromField)),
      _connectToField(std::move(connectToField)),
      _startWith(std::move(startWith)),
      _additionalFilter(std::move(additionalFilter)),
      _depthField(std::move(depthField)),
      _maxDepth(maxDepth) {
    if (fromPipeline) {
        _pipelines.push_back(std::move(*fromPipeline));
    }
}

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

    boost::optional<BSONObj> startWith;
    if (const auto& sw = parsedSpec.getStartWith()) {
        startWith = sw->getElement().wrap().getOwned();
    }

    // $_internalFromPipeline is an internal field set by mongos when dispatching to shards.
    // Reject it from external clients.
    boost::optional<OwnedLiteParsedPipeline> fromPipeline;
    if (const auto& fromPipelineStages = parsedSpec.getInternalFromPipeline()) {
        if (options.opCtx) {
            assertAllowedInternalIfRequired(
                options.opCtx,
                DocumentSourceGraphLookUpSpec::kInternalFromPipelineFieldName,
                AllowedWithClientType::kInternal);
        }
        fromPipeline.emplace(foreignNss, *fromPipelineStages, options);
    }

    return std::make_unique<LiteParsedGraphLookUp>(spec,
                                                   std::move(foreignNss),
                                                   std::move(as),
                                                   std::move(connectFromField),
                                                   std::move(connectToField),
                                                   std::move(startWith),
                                                   std::move(additionalFilter),
                                                   std::move(depthField),
                                                   maxDepth,
                                                   options,
                                                   std::move(fromPipeline));
}

PrivilegeVector LiteParsedGraphLookUp::requiredPrivileges(bool isMongos,
                                                          bool bypassDocumentValidation) const {
    // find on the 'from' namespace is the correct and complete required privilege. MongoDB's view
    // access control model gates access at the view boundary: find on the view is sufficient, and
    // the view's underlying pipeline stages are checked at view creation time, not query time.
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
    boost::optional<OwnedLiteParsedPipeline> lpp;
    if (!_pipelines.empty()) {
        lpp.emplace(_pipelines[0]);
    }
    boost::optional<BSONElement> startWithElem;
    if (_startWith) {
        startWithElem = _startWith->firstElement();
    }
    return std::make_unique<GraphLookUpStageParams>(*_foreignNss,
                                                    _as,
                                                    _connectFromField,
                                                    _connectToField,
                                                    startWithElem,
                                                    _additionalFilter,
                                                    _depthField,
                                                    _maxDepth,
                                                    getOriginalBson(),
                                                    std::move(lpp));
}

}  // namespace mongo
