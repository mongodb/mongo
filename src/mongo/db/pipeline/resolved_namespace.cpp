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

#include "mongo/db/pipeline/resolved_namespace.h"

#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {

void ResolvedNamespace::setViewPipelineDesugarer(ViewPipelineDesugarer fn) {
    _viewPipelineDesugarer = std::move(fn);
}

const ResolvedNamespaceViewOptions kSimpleViewOptions = ResolvedNamespaceViewOptions{
    .involvedNamespaceIsAView = true,
};

ResolvedNamespace::ResolvedNamespace() = default;

ResolvedNamespace::ResolvedNamespace(NamespaceString ns_,
                                     std::vector<BSONObj> pipeline_,
                                     boost::optional<UUID> collUUID_,
                                     bool involvedNamespaceIsAView_)
    : ns(ns_),
      pipeline(std::move(pipeline_)),
      uuid(collUUID_),
      involvedNamespaceIsAView(involvedNamespaceIsAView_),
      _userNss(ns_) {}

ResolvedNamespace::ResolvedNamespace(NamespaceString userNss,
                                     NamespaceString resolvedNss,
                                     std::vector<BSONObj> pipeline_,
                                     BSONObj defaultCollation,
                                     ResolvedNamespaceViewOptions metadata)
    : ns(std::move(resolvedNss)),
      pipeline(std::move(pipeline_)),
      uuid(metadata.collUUID),
      involvedNamespaceIsAView(metadata.involvedNamespaceIsAView),
      _userNss(std::move(userNss)),
      _defaultCollation(std::move(defaultCollation)),
      _timeseriesMetadata(metadata.timeseriesMetadata),
      _lpOptions(std::move(metadata.options)) {
    if (metadata.validateIsNotViewlessTimeseries) {
        // If we reach here with a timeseries query, it will be because we're working with a
        // view-based timeseries collection. Viewless timeseries collections should be defined
        // already and should not trigger this kickback at all.
        //
        // TODO(SERVER-100862): This check should be removed once the isNewTimeseriesWithoutView
        // parameter has been removed.
        tassert(
            9950300,
            fmt::format(
                "Should not be performing view resolution on viewless timeseries collection: {}",
                ns.toStringForErrorMsg()),
            !metadata.isViewlessTimeseries);
    }

    if (metadata.shouldParseLpp && involvedNamespaceIsAView) {
        liteParseViewPipeline();
    }
}

// Copy constructor. Should be used carefully when _parsedPipeline is set to avoid unnecessary
// clones.
ResolvedNamespace::ResolvedNamespace(const ResolvedNamespace& other)
    : ns(other.ns),
      pipeline(other.pipeline),
      uuid(other.uuid),
      involvedNamespaceIsAView(other.involvedNamespaceIsAView),
      _userNss(other._userNss),
      _defaultCollation(other._defaultCollation),
      _timeseriesMetadata(other._timeseriesMetadata),
      _lpOptions(other._lpOptions) {
    if (other._parsedPipeline) {
        _parsedPipeline = std::make_unique<LiteParsedPipeline>(other._parsedPipeline->clone());
    }
}

// Copy constructor. Should be used carefully when _parsedPipeline is set to avoid unnecessary
// clones.
ResolvedNamespace& ResolvedNamespace::operator=(const ResolvedNamespace& other) {
    if (this == &other)
        return *this;
    ns = other.ns;
    pipeline = other.pipeline;
    uuid = other.uuid;
    involvedNamespaceIsAView = other.involvedNamespaceIsAView;
    _userNss = other._userNss;
    _defaultCollation = other._defaultCollation;
    _timeseriesMetadata = other._timeseriesMetadata;
    _lpOptions = other._lpOptions;
    if (other._parsedPipeline) {
        _parsedPipeline = std::make_unique<LiteParsedPipeline>(other._parsedPipeline->clone());
    } else {
        _parsedPipeline = nullptr;
    }
    return *this;
}

ResolvedNamespace::ResolvedNamespace(ResolvedNamespace&& other) noexcept
    : ns(std::move(other.ns)),
      pipeline(std::move(other.pipeline)),
      uuid(std::move(other.uuid)),
      involvedNamespaceIsAView(other.involvedNamespaceIsAView),
      _userNss(std::move(other._userNss)),
      _defaultCollation(std::move(other._defaultCollation)),
      _timeseriesMetadata(std::move(other._timeseriesMetadata)),
      _lpOptions(std::move(other._lpOptions)),
      _parsedPipeline(std::move(other._parsedPipeline)) {}

ResolvedNamespace& ResolvedNamespace::operator=(ResolvedNamespace&& other) noexcept {
    if (this == &other)
        return *this;
    ns = std::move(other.ns);
    pipeline = std::move(other.pipeline);
    uuid = std::move(other.uuid);
    involvedNamespaceIsAView = other.involvedNamespaceIsAView;
    _userNss = std::move(other._userNss);
    _defaultCollation = std::move(other._defaultCollation);
    _timeseriesMetadata = std::move(other._timeseriesMetadata);
    _lpOptions = std::move(other._lpOptions);
    _parsedPipeline = std::move(other._parsedPipeline);
    return *this;
}

ResolvedNamespace::~ResolvedNamespace() = default;

const NamespaceString& ResolvedNamespace::getNamespace() const {
    return _userNss;
}

const NamespaceString& ResolvedNamespace::getResolvedNamespace() const {
    return ns;
}

const std::vector<BSONObj>& ResolvedNamespace::getBsonPipeline() const {
    return pipeline;
}

const BSONObj& ResolvedNamespace::getDefaultCollation() const {
    return _defaultCollation;
}

bool ResolvedNamespace::isTimeseries() const {
    return _timeseriesMetadata.has_value() && _timeseriesMetadata->options.has_value();
}

boost::optional<TimeseriesViewMetadata> ResolvedNamespace::getTimeseriesViewMetadata() const {
    return _timeseriesMetadata;
}

void ResolvedNamespace::liteParseViewPipeline() {
    LiteParserOptions optsToUse = _lpOptions ? *_lpOptions : LiteParserOptions();
    for (auto& bson : pipeline) {
        bson = bson.getOwned();
    }
    // TODO SERVER-117525 Use an OwnedLiteParsedPipeline here.
    _parsedPipeline = std::make_unique<LiteParsedPipeline>(ns, pipeline, false, optsToUse);
    _parsedPipeline->makeOwned();
}

void ResolvedNamespace::desugarViewPipeline() {
    // _viewPipelineDesugarer is registered at startup by lite_parsed_desugarer.cpp. See the
    // ViewPipelineDesugarer typedef comment in resolved_namespace.h for why this is a callback.
    if (_parsedPipeline && _viewPipelineDesugarer) {
        _viewPipelineDesugarer(_parsedPipeline.get());
    }
}

LiteParsedPipeline ResolvedNamespace::getViewPipeline() const {
    tassert(11506600,
            "ResolvedNamespace must be parsed before calling `getViewPipeline()`",
            _parsedPipeline);

    // Ownership should be preserved because the original stages own their BSON, and the default
    // copy constructor copies _ownedBson, which uses BSONObj's shared ownership. Still, we call
    // makeOwned() defensively here. This is a no-op if the stages already own their BSON.
    auto out = _parsedPipeline->clone();
    out.makeOwned();
    return out;
}

std::vector<BSONObj> ResolvedNamespace::getOriginalBson() const {
    return pipeline;
}

std::vector<BSONObj> ResolvedNamespace::getSerializedViewPipeline() const {
    tassert(11898700,
            "A ResolvedNamespace must have a parsed view pipeline to get desugared BSON.",
            _parsedPipeline);

    std::vector<BSONObj> result;
    result.reserve(_parsedPipeline->getStages().size());
    for (const auto& stage : _parsedPipeline->getStages()) {
        result.push_back(stage->getOriginalBson().wrap());
    }
    return result;
}

ResolvedNamespace ResolvedNamespace::clone() const {
    return ResolvedNamespace(*this);
}

ResolvedNamespace ResolvedNamespace::fromBSON(const BSONObj& commandResponseObj) {
    uassert(40248,
            "command response expected to have a 'resolvedView' field",
            commandResponseObj.hasField("resolvedView"));

    auto viewDef = commandResponseObj.getObjectField("resolvedView");
    uassert(40249, "resolvedView must be an object", !viewDef.isEmpty());

    uassert(40250,
            "View definition must have 'ns' field of type string",
            viewDef.hasField("ns") && viewDef.getField("ns").type() == BSONType::string);

    uassert(40251,
            "View definition must have 'pipeline' field of type array",
            viewDef.hasField("pipeline") && viewDef.getField("pipeline").type() == BSONType::array);

    std::vector<BSONObj> pipeline;
    for (auto&& item : viewDef["pipeline"].Obj()) {
        pipeline.push_back(item.Obj().getOwned());
    }

    BSONObj collationSpec;
    if (auto collationElt = viewDef["collation"]) {
        uassert(40639,
                "View definition 'collation' field must be an object",
                collationElt.type() == BSONType::object);
        collationSpec = collationElt.embeddedObject().getOwned();
    }

    boost::optional<TimeseriesOptions> timeseriesOptions = boost::none;
    if (auto tsOptionsElt = viewDef[TimeseriesViewMetadata::kTimeseriesOptions]) {
        if (tsOptionsElt.isABSONObj()) {
            timeseriesOptions = TimeseriesOptions::parse(
                tsOptionsElt.Obj(), IDLParserContext{"ResolvedNamespace::fromBSON"});
        }
    }

    boost::optional<bool> mixedSchema = boost::none;
    if (auto mixedSchemaElem = viewDef[TimeseriesViewMetadata::kTimeseriesMayContainMixedData]) {
        uassert(6067204,
                str::stream() << "view definition must have "
                              << TimeseriesViewMetadata::kTimeseriesMayContainMixedData
                              << " of type bool or no such field",
                mixedSchemaElem.type() == BSONType::boolean);

        mixedSchema = boost::optional<bool>(mixedSchemaElem.boolean());
    }

    boost::optional<bool> usesExtendedRange = boost::none;
    if (auto usesExtendedRangeElem =
            viewDef[TimeseriesViewMetadata::kTimeseriesUsesExtendedRange]) {
        uassert(6646910,
                str::stream() << "view definition must have "
                              << TimeseriesViewMetadata::kTimeseriesUsesExtendedRange
                              << " of type bool or no such field",
                usesExtendedRangeElem.type() == BSONType::boolean);

        usesExtendedRange = boost::optional<bool>(usesExtendedRangeElem.boolean());
    }

    boost::optional<bool> fixedBuckets = boost::none;
    if (auto fixedBucketsElem = viewDef[TimeseriesViewMetadata::kTimeseriesfixedBuckets]) {
        uassert(7823304,
                str::stream() << "view definition must have "
                              << TimeseriesViewMetadata::kTimeseriesfixedBuckets
                              << " of type bool or no such field",
                fixedBucketsElem.type() == BSONType::boolean);

        fixedBuckets = boost::optional<bool>(fixedBucketsElem.boolean());
    }

    TimeseriesViewMetadata tsMetadata{.options = std::move(timeseriesOptions),
                                      .mayContainMixedData = std::move(mixedSchema),
                                      .usesExtendedRange = std::move(usesExtendedRange),
                                      .fixedBuckets = std::move(fixedBuckets)};

    ResolvedNamespaceViewOptions options;
    options.validateIsNotViewlessTimeseries = true;
    options.timeseriesMetadata = std::move(tsMetadata);

    NamespaceString resolvedNss =
        NamespaceStringUtil::deserializeForErrorMsg(viewDef["ns"].valueStringData());
    NamespaceString userNss = [&] {
        // Try to parse `userNs` out of BSON spec. If not provided, fallback to `resolvedNss`.
        if (auto userNsElt = viewDef["userNs"]; userNsElt && userNsElt.type() == BSONType::string) {
            return NamespaceStringUtil::deserializeForErrorMsg(userNsElt.valueStringData());
        }
        return resolvedNss;
    }();

    return ResolvedNamespace{std::move(userNss),
                             std::move(resolvedNss),
                             std::move(pipeline),
                             std::move(collationSpec),
                             options};
}

void TimeseriesViewMetadata::serialize(BSONObjBuilder* optionsBuilder,
                                       BSONObjBuilder* subObjBuilder) const {
    if (options) {
        BSONObjBuilder tsObj(optionsBuilder->subobjStart(kTimeseriesOptions));
        options->serialize(&tsObj);
    }
    // Only serialize if it doesn't contain mixed data.
    if ((mayContainMixedData && !(*mayContainMixedData)))
        subObjBuilder->append(kTimeseriesMayContainMixedData, *mayContainMixedData);

    if ((usesExtendedRange && (*usesExtendedRange)))
        subObjBuilder->append(kTimeseriesUsesExtendedRange, *usesExtendedRange);

    if ((fixedBuckets && (*fixedBuckets)))
        subObjBuilder->append(kTimeseriesfixedBuckets, *fixedBuckets);
}


void ResolvedNamespace::serialize(BSONObjBuilder* builder) const {
    BSONObjBuilder subObj(builder->subobjStart("resolvedView"));
    subObj.append("ns", ns.toStringForErrorMsg());
    if (_userNss != ns) {
        subObj.append("userNs", _userNss.toStringForErrorMsg());
    }
    subObj.append("pipeline", pipeline);
    if (_timeseriesMetadata.has_value()) {
        _timeseriesMetadata->serialize(builder, &subObj);
    }

    if (!_defaultCollation.isEmpty()) {
        subObj.append("collation", _defaultCollation);
    }
}

std::shared_ptr<const ResolvedNamespace> ResolvedNamespace::parse(const BSONObj& cmdReply) {
    return std::make_shared<ResolvedNamespace>(fromBSON(cmdReply));
}

ResolvedNamespace ResolvedNamespace::parseFromBSON(const BSONElement& elem) {
    uassert(936370, "resolvedView must be an object", elem.type() == BSONType::object);
    BSONObjBuilder localBuilder;
    localBuilder.append("resolvedView", elem.Obj());
    return fromBSON(localBuilder.done());
}

void ResolvedNamespace::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    serialize(builder);
}

}  // namespace mongo
