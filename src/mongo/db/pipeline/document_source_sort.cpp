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

#include "mongo/db/pipeline/document_source_sort.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <iterator>
#include <list>
#include <tuple>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {
struct BoundMakerMin {
    const long long offset;  // Offset in millis

    DocumentSourceSort::SortableDate operator()(DocumentSourceSort::SortableDate key,
                                                const Document& doc) const {
        return {Date_t::fromMillisSinceEpoch(
            doc.metadata().getTimeseriesBucketMinTime().toMillisSinceEpoch() + offset)};
    }

    Document serialize(const SerializationOptions& opts) const {
        // Convert from millis to seconds.
        return Document{{{"base"_sd, DocumentSourceSort::kMin},
                         {DocumentSourceSort::kOffset, opts.serializeLiteral(offset / 1000)}}};
    }
};

struct BoundMakerMax {
    const long long offset;

    DocumentSourceSort::SortableDate operator()(DocumentSourceSort::SortableDate key,
                                                const Document& doc) const {
        return {Date_t::fromMillisSinceEpoch(
            doc.metadata().getTimeseriesBucketMaxTime().toMillisSinceEpoch() + offset)};
    }

    Document serialize(const SerializationOptions& opts) const {
        // Convert from millis to seconds.
        return Document{{{"base"_sd, DocumentSourceSort::kMax},
                         {DocumentSourceSort::kOffset, opts.serializeLiteral(offset / 1000)}}};
    }
};
struct CompAsc {
    int operator()(DocumentSourceSort::SortableDate x, DocumentSourceSort::SortableDate y) const {
        // compare(x, y) op 0 means x op y, for any comparator 'op'.
        if (x.date.toMillisSinceEpoch() < y.date.toMillisSinceEpoch())
            return -1;
        if (x.date.toMillisSinceEpoch() > y.date.toMillisSinceEpoch())
            return 1;
        return 0;
    }
};
struct CompDesc {
    int operator()(DocumentSourceSort::SortableDate x, DocumentSourceSort::SortableDate y) const {
        // compare(x, y) op 0 means x op y, for any comparator 'op'.
        if (x.date.toMillisSinceEpoch() > y.date.toMillisSinceEpoch())
            return -1;
        if (x.date.toMillisSinceEpoch() < y.date.toMillisSinceEpoch())
            return 1;
        return 0;
    }
};

[[noreturn]] void throwCannotHandleControlEvent() {
    tasserted(10358905, "Sort does not support control events");
}

using TimeSorterAscMin =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompAsc, BoundMakerMin>;
using TimeSorterAscMax =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompAsc, BoundMakerMax>;
using TimeSorterDescMin =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompDesc, BoundMakerMin>;
using TimeSorterDescMax =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompDesc, BoundMakerMax>;
}  // namespace

const DocumentSourceSort::SortStageOptions DocumentSourceSort::kDefaultOptions = {};

DocumentSourceSort::DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       const SortPattern& sortOrder,
                                       DocumentSourceSort::SortStageOptions options)
    : DocumentSource(kStageName, pExpCtx),
      exec::agg::Stage(kStageName, pExpCtx),
      _sortExecutor({sortOrder,
                     options.limit,
                     options.maxMemoryUsageBytes.value_or(
                         internalQueryMaxBlockingSortMemoryUsageBytes.load()),
                     pExpCtx->getTempDir(),
                     pExpCtx->getAllowDiskUse()}),
      // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
      // by a metadata field.
      _sortKeyGen({sortOrder, pExpCtx->getCollator()}),
      _outputSortKeyMetadata(options.outputSortKeyMetadata) {
    uassert(15976,
            "$sort stage must have at least one sort key",
            !_sortExecutor->sortPattern().empty());
}

REGISTER_DOCUMENT_SOURCE(sort,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSort::createFromBson,
                         AllowedWithApiStrict::kAlways);

ALLOCATE_DOCUMENT_SOURCE_ID(sort, DocumentSourceSort::id)

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(_internalBoundedSort,
                                       LiteParsedDocumentSourceDefault::parse,
                                       DocumentSourceSort::parseBoundedSort,
                                       ::mongo::getTestCommandsEnabled()
                                           ? AllowedWithApiStrict::kNeverInVersion1
                                           : AllowedWithApiStrict::kInternal,
                                       ::mongo::getTestCommandsEnabled()
                                           ? AllowedWithClientType::kAny
                                           : AllowedWithClientType::kInternal,
                                       nullptr,  // featureFlag
                                       true);

DocumentSource::GetNextResult::ReturnStatus DocumentSourceSort::timeSorterPeek() {
    if (_timeSorterNextDoc) {
        return GetNextResult::ReturnStatus::kAdvanced;
    }
    if (_timeSorterInputEOF) {
        return GetNextResult::ReturnStatus::kEOF;
    }

    auto next = pSource->getNext();
    auto status = next.getStatus();
    switch (status) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            _timeSorterNextDoc = next.getDocument();
            return status;
        }
        case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            throwCannotHandleControlEvent();
        }
        case GetNextResult::ReturnStatus::kEOF:
            _timeSorterInputEOF = true;
            return status;
        case GetNextResult::ReturnStatus::kPauseExecution:
            return status;
    }
    MONGO_UNREACHABLE_TASSERT(6434800);
}

Document DocumentSourceSort::timeSorterGetNext() {
    tassert(6434801,
            "timeSorterGetNext() is only valid after timeSorterPeek() returns isAdvanced()",
            _timeSorterNextDoc);
    auto result = std::move(*_timeSorterNextDoc);
    _timeSorterNextDoc.reset();
    return result;
}

DocumentSource::GetNextResult::ReturnStatus DocumentSourceSort::timeSorterPeekSamePartition() {
    auto status = timeSorterPeek();
    switch (status) {
        case GetNextResult::ReturnStatus::kEOF:
        case GetNextResult::ReturnStatus::kPauseExecution:
            return status;
        case GetNextResult::ReturnStatus::kAdvanced:
            break;
        case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            throwCannotHandleControlEvent();
        }
    }

    if (!_timeSorterPartitionKeyGen) {
        // No partition key means everything is in the same partition.
        return GetNextResult::ReturnStatus::kAdvanced;
    } else {
        auto prevPartition = std::move(_timeSorterCurrentPartition);
        _timeSorterCurrentPartition =
            _timeSorterPartitionKeyGen->computeSortKeyFromDocument(*_timeSorterNextDoc);

        if (!prevPartition) {
            // No previous partition means there is no constraint.
            return GetNextResult::ReturnStatus::kAdvanced;
        } else if (pExpCtx->getValueComparator().evaluate(*_timeSorterCurrentPartition ==
                                                          *prevPartition)) {
            // Next is in the same partition.
            return GetNextResult::ReturnStatus::kAdvanced;
        } else {
            // Next is in a new partition: pretend we don't have a next document.
            return GetNextResult::ReturnStatus::kEOF;
        }
    }
}

DocumentSource::GetNextResult DocumentSourceSort::doGetNext() {
    if (_timeSorter) {
        // If the _timeSorter is exhausted but we have more input, it must be because we just
        // finished a partition. Restart the _timeSorter to make it ready for the next partition.
        if (_timeSorter->getState() == TimeSorterInterface::State::kDone &&
            timeSorterPeek() == GetNextResult::ReturnStatus::kAdvanced) {
            updateTimeSorterStats();
            _timeSorter->restart();
            _timeSorterCurrentPartition.reset();
        }

        // Only pull input as necessary to get _timeSorter to have a result.
        while (_timeSorter->getState() == TimeSorterInterface::State::kWait) {
            auto status = timeSorterPeekSamePartition();
            switch (status) {
                case GetNextResult::ReturnStatus::kPauseExecution: {
                    return GetNextResult::makePauseExecution();
                }
                case GetNextResult::ReturnStatus::kEOF: {
                    // We've reached the end of the current partition. Tell _timeSorter there will
                    // be no more input. In response, its state will never be kWait again unless we
                    // restart it, so we can proceed to drain all the documents currently held by
                    // the sorter.
                    _timeSorter->done();
                    tassert(
                        6434802,
                        "DocumentSourceSort::_timeSorter waiting for input that will not arrive",
                        _timeSorter->getState() != TimeSorterInterface::State::kWait);
                    continue;
                }
                case GetNextResult::ReturnStatus::kAdvanced: {
                    auto [time, doc] = extractTime(timeSorterGetNext());

                    _timeSorter->add({time}, doc);
                    continue;
                }
                case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
                    throwCannotHandleControlEvent();
                }
            }
        }

        if (_timeSorter->getState() == TimeSorterInterface::State::kDone) {
            updateTimeSorterStats();
            return GetNextResult::makeEOF();
        }

        return _timeSorter->next().second;
    }

    if (!_populated) {
        auto populationResult = populate();
        if (MONGO_unlikely(populationResult.isAdvancedControlDocument())) {
            throwCannotHandleControlEvent();
        }
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());
    }

    if (!_sortExecutor->hasNext()) {
        return GetNextResult::makeEOF();
    }

    return GetNextResult{_sortExecutor->getNext().second};
}

void DocumentSourceSort::serializeForBoundedSort(std::vector<Value>& array,
                                                 const SerializationOptions& opts) const {
    tassert(9028700, "_timeSorter is nullptr", _timeSorter);
    tassert(
        6369900, "$_internalBoundedSort should not absorb a $limit", !_sortExecutor->hasLimit());

    // {$_internalBoundedSort: {sortKey, bound}}
    auto sortKey = _sortExecutor->sortPattern().serialize(
        SortPattern::SortKeySerialization::kForPipelineSerialization, opts);

    MutableDocument mutDoc{Document{{
        {"$_internalBoundedSort"_sd,
         Document{
             {{"sortKey"_sd, std::move(sortKey)},
              {"bound"_sd, _timeSorter->serializeBound(opts)},
              {"limit"_sd, opts.serializeLiteral(static_cast<long long>(_timeSorter->limit()))}}}},
    }}};

    if (opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        mutDoc["totalDataSizeSortedBytesEstimate"] =
            opts.serializeLiteral(static_cast<long long>(_timeSorter->stats().bytesSorted()));
        mutDoc["usedDisk"] = opts.serializeLiteral(_timeSorter->stats().spilledRanges() > 0);
        mutDoc["spills"] =
            opts.serializeLiteral(static_cast<long long>(_timeSorter->stats().spilledRanges()));
        mutDoc["spilledDataStorageSize"] =
            opts.serializeLiteral(static_cast<long long>(_sortExecutor->spilledDataStorageSize()));
    }

    array.push_back(Value{mutDoc.freeze()});
}

void DocumentSourceSort::serializeForCloning(std::vector<Value>& array,
                                             const SerializationOptions& opts) const {
    MutableDocument mutDoc(_sortExecutor->sortPattern().serialize(
        SortPattern::SortKeySerialization::kForPipelineSerialization, opts));
    if (_sortExecutor->hasLimit()) {
        mutDoc[kInternalLimit] =
            opts.serializeLiteral(static_cast<long long>(_sortExecutor->getLimit()));
    }
    if (_outputSortKeyMetadata) {
        mutDoc[kInternalOutputSortKey] = Value(_outputSortKeyMetadata);
    }
    array.push_back(Value(DOC(kStageName << mutDoc.freeze())));
}

void DocumentSourceSort::serializeWithVerbosity(std::vector<Value>& array,
                                                const SerializationOptions& opts) const {
    tassert(9028701, "SerializationOptions do not specify verbosity", opts.verbosity);

    uint64_t limit = _sortExecutor->getLimit();
    MutableDocument mutDoc(
        DOC(kStageName << DOC(
                "sortKey"
                << _sortExecutor->sortPattern().serialize(
                       SortPattern::SortKeySerialization::kForExplain, opts)
                // Only output 'limit' and 'outputSortKeyMetadata' if they're true, mostly to
                // preserve conciseness for the simple cases without loss of information. It
                // also prevents the need to update some tests and potentially saves some
                // downstream changes work if there are similar tests elsewhere.
                << "limit"
                << (_sortExecutor->hasLimit() ? opts.serializeLiteral(static_cast<long long>(limit))
                                              : Value())
                << "outputSortKeyMetadata" << (_outputSortKeyMetadata ? Value(true) : Value()))));

    if (opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        auto& stats = _sortExecutor->stats();

        mutDoc["totalDataSizeSortedBytesEstimate"] =
            opts.serializeLiteral(static_cast<long long>(stats.totalDataSizeBytes));
        mutDoc["usedDisk"] = opts.serializeLiteral(stats.spillingStats.getSpills() > 0);
        mutDoc["spills"] =
            opts.serializeLiteral(static_cast<long long>(stats.spillingStats.getSpills()));
        mutDoc["spilledDataStorageSize"] =
            opts.serializeLiteral(static_cast<long long>(_sortExecutor->spilledDataStorageSize()));
    }
    array.push_back(Value(mutDoc.freeze()));
}

void DocumentSourceSort::serializeToArray(std::vector<Value>& array,
                                          const SerializationOptions& opts) const {
    if (_timeSorter) {
        serializeForBoundedSort(array, opts);
    } else if (opts.isSerializingForExplain()) {
        serializeWithVerbosity(array, opts);
    } else if (opts.serializeForCloning) {
        serializeForCloning(array, opts);
    } else {
        MutableDocument mutDoc(_sortExecutor->sortPattern().serialize(
            SortPattern::SortKeySerialization::kForPipelineSerialization, opts));
        // If we are serializing for query shape, omit this field in order to maintain stabiilty.
        if (_outputSortKeyMetadata && opts.isKeepingLiteralsUnchanged()) {
            mutDoc[kInternalOutputSortKey] = Value(_outputSortKeyMetadata);
        }
        array.push_back(Value(DOC(kStageName << mutDoc.freeze())));
        if (_sortExecutor->hasLimit()) {
            auto limitSrc = DocumentSourceLimit::create(pExpCtx, _sortExecutor->getLimit());
            limitSrc->serializeToArray(array, opts);
        }
    }
}

boost::optional<long long> DocumentSourceSort::getLimit() const {
    return _sortExecutor->hasLimit() ? boost::optional<long long>{_sortExecutor->getLimit()}
                                     : boost::none;
}

DocumentSourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (_timeSorter) {
        // Do not absorb a limit, or combine with other sort stages.
        return std::next(itr);
    }

    auto stageItr = std::next(itr);
    auto limit = extractLimitForPushdown(stageItr, container);
    if (limit)
        _sortExecutor->setLimit(*limit);

    auto nextStage = std::next(itr);
    if (nextStage == container->end()) {
        return container->end();
    }

    limit = getLimit();

    // Since $sort is not guaranteed to be stable, we can blindly remove the first $sort only when
    // there's no limit on the current sort.
    if (auto nextSort = dynamic_cast<DocumentSourceSort*>((*nextStage).get())) {
        // Ensure that we don't accidentally erase the request to output the sort key metadata.
        nextSort->_outputSortKeyMetadata |= _outputSortKeyMetadata;
        if (!limit) {
            container->erase(itr);
            return nextStage;
        } else {
            // If there's a limit between two adjacent sorts with the same key pattern it's safe to
            // merge the two sorts and take the minimum of the limits.
            if (dynamic_cast<DocumentSourceSort*>((*itr).get())->getSortKeyPattern() ==
                nextSort->getSortKeyPattern()) {
                // When coalescing subsequent $sort stages, the existing/lower limit is retained in
                // 'setLimit'.
                nextSort->_sortExecutor->setLimit(*limit);
                container->erase(itr);
            }
        }
    }
    return nextStage;
}

DepsTracker::State DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    _sortExecutor->sortPattern().addDependencies(deps);

    if (_requiredMetadata.any()) {
        deps->setNeedsMetadata(_requiredMetadata);
    }

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceSort::addVariableRefs(std::set<Variables::Id>* refs) const {
    // It's impossible for $sort or the find command's sort to refer to a variable.
}

boost::intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(
        15973, "the $sort key specification must be an object", spec.type() == BSONType::object);

    auto specObj = spec.Obj();
    // Reconstruct the sort order without these internal fields.
    BSONObjBuilder sortOrder;

    auto options = kDefaultOptions;
    for (auto&& elem : specObj) {
        if (elem.fieldNameStringData() == kInternalLimit) {
            const auto limitStatus = elem.parseIntegerElementToNonNegativeLong();
            uassert(9028702,
                    str::stream() << "invalid $_internalLimit value: "
                                  << limitStatus.getStatus().reason(),
                    limitStatus.isOK());
            options.limit = limitStatus.getValue();
        } else if (elem.fieldNameStringData() == kInternalOutputSortKey) {
            options.outputSortKeyMetadata = elem.Bool();
        } else {
            sortOrder.append(elem);
        }
    }
    return create(pExpCtx, {sortOrder.obj(), pExpCtx}, std::move(options));
}

boost::intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const SortPattern& sortOrder,
    SortStageOptions options) {
    return make_intrusive<DocumentSourceSort>(pExpCtx, sortOrder, std::move(options));
}

boost::intrusive_ptr<DocumentSourceSort> DocumentSourceSort::createBoundedSort(
    SortPattern pat,
    StringData boundBase,
    long long boundOffset,
    boost::optional<long long> limit,
    bool outputSortKeyMetadata,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    auto ds = DocumentSourceSort::create(expCtx, pat);

    SortOptions opts;
    opts.maxMemoryUsageBytes = internalQueryMaxBlockingSortMemoryUsageBytes.load();
    if (expCtx->getAllowDiskUse()) {
        opts.extSortAllowed = true;
        opts.tempDir = expCtx->getTempDir();
        opts.sorterFileStats = ds->getSorterFileStats();
    }

    if (limit) {
        opts.Limit(limit.value());
    }

    if (boundBase == kMin) {
        if (pat.back().isAscending) {
            ds->_timeSorter.reset(
                new TimeSorterAscMin{opts, CompAsc{}, BoundMakerMin{boundOffset}});
        } else {
            ds->_timeSorter.reset(
                new TimeSorterDescMin{opts, CompDesc{}, BoundMakerMin{boundOffset}});
        }
        ds->_requiredMetadata.set(DocumentMetadataFields::MetaType::kTimeseriesBucketMinTime);
    } else if (boundBase == kMax) {
        if (pat.back().isAscending) {
            ds->_timeSorter.reset(
                new TimeSorterAscMax{opts, CompAsc{}, BoundMakerMax{boundOffset}});
        } else {
            ds->_timeSorter.reset(
                new TimeSorterDescMax{opts, CompDesc{}, BoundMakerMax{boundOffset}});
        }
        ds->_requiredMetadata.set(DocumentMetadataFields::MetaType::kTimeseriesBucketMaxTime);
    } else {
        MONGO_UNREACHABLE;
    }

    ds->_outputSortKeyMetadata = outputSortKeyMetadata;

    if (pat.size() > 1) {
        SortPattern partitionKey =
            std::vector<SortPattern::SortPatternPart>(pat.begin(), pat.end() - 1);
        ds->_timeSorterPartitionKeyGen =
            SortKeyGenerator{std::move(partitionKey), expCtx->getCollator()};
    }

    ds->_timeSorterStats = ds->_sortExecutor->stats();

    return ds;
}

boost::intrusive_ptr<DocumentSourceSort> DocumentSourceSort::parseBoundedSort(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6369905,
            "the $_internalBoundedSort key specification must be an object",
            elem.type() == BSONType::object);
    BSONObj args = elem.embeddedObject();

    BSONElement key = args["sortKey"];
    uassert(
        6369904, "$_internalBoundedSort sortKey must be an object", key.type() == BSONType::object);

    // Empty sort pattern is not allowed for the bounded sort.
    uassert(6900501,
            "$_internalBoundedSort stage must have at least one sort key",
            !key.embeddedObject().isEmpty());
    SortPattern pat{key.embeddedObject(), expCtx};

    {
        const auto& timePart = pat.back();
        uassert(6369901,
                "$_internalBoundedSort doesn't support an expression in the time field (the last "
                "component of sortKey)",
                timePart.expression == nullptr);
        uassert(6369907,
                "$_internalBoundedSort doesn't support dotted field names in the time field (the "
                "last component of sortKey)",
                timePart.fieldPath->getPathLength() == 1);
    }

    BSONElement bound = args["bound"];
    uassert(6460200,
            "$_internalBoundedSort bound must be an object",
            bound && bound.type() == BSONType::object);

    BSONElement boundOffsetElem = bound.Obj()[DocumentSourceSort::kOffset];
    long long boundOffset = 0;
    if (boundOffsetElem && boundOffsetElem.isNumber()) {
        boundOffset = uassertStatusOK(boundOffsetElem.parseIntegerElementToLong()) *
            1000;  // convert to millis
    }

    BSONElement boundBaseElem = bound.Obj()["base"];
    uassert(6460201,
            "$_internalBoundedSort bound.base must be a string",
            boundBaseElem && boundBaseElem.type() == BSONType::string);
    StringData boundBase = boundBaseElem.valueStringData();
    uassert(6460202,
            str::stream() << "$_internalBoundedSort bound.base must be '" << kMin << "' or '"
                          << kMax << "'",
            boundBase == kMin || boundBase == kMax);

    auto ds = DocumentSourceSort::create(expCtx, pat);

    SortOptions opts;
    opts.MaxMemoryUsageBytes(internalQueryMaxBlockingSortMemoryUsageBytes.load());
    if (expCtx->getAllowDiskUse()) {
        opts.ExtSortAllowed(true);
        opts.TempDir(expCtx->getTempDir());
        opts.FileStats(ds->getSorterFileStats());
    }
    if (BSONElement limitElem = args["limit"]) {
        uassert(6588100,
                "$_internalBoundedSort limit must be a non-negative number if specified",
                limitElem.isNumber() && limitElem.numberLong() >= 0);
        opts.Limit(limitElem.numberLong());
    }

    if (boundBase == kMin) {
        if (pat.back().isAscending) {
            ds->_timeSorter.reset(
                new TimeSorterAscMin{opts, CompAsc{}, BoundMakerMin{boundOffset}});
        } else {
            ds->_timeSorter.reset(
                new TimeSorterDescMin{opts, CompDesc{}, BoundMakerMin{boundOffset}});
        }
        ds->_requiredMetadata.set(DocumentMetadataFields::MetaType::kTimeseriesBucketMinTime);
    } else if (boundBase == kMax) {
        if (pat.back().isAscending) {
            ds->_timeSorter.reset(
                new TimeSorterAscMax{opts, CompAsc{}, BoundMakerMax{boundOffset}});
        } else {
            ds->_timeSorter.reset(
                new TimeSorterDescMax{opts, CompDesc{}, BoundMakerMax{boundOffset}});
        }
        ds->_requiredMetadata.set(DocumentMetadataFields::MetaType::kTimeseriesBucketMaxTime);
    } else {
        MONGO_UNREACHABLE;
    }

    if (pat.size() > 1) {
        SortPattern partitionKey =
            std::vector<SortPattern::SortPatternPart>(pat.begin(), pat.end() - 1);
        ds->_timeSorterPartitionKeyGen =
            SortKeyGenerator{std::move(partitionKey), expCtx->getCollator()};
    }

    return ds;
}

DocumentSource::GetNextResult DocumentSourceSort::populate() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        loadDocument(nextInput.releaseDocument());
    }
    if (nextInput.isEOF()) {
        loadingDone();
    }
    return nextInput;
}

void DocumentSourceSort::loadDocument(Document&& doc) {
    invariant(!_populated);

    Value sortKey;
    Document docForSorter;
    // We always need to extract the sort key if we've reached this point. If the query system had
    // already computed the sort key we'd have split the pipeline there, would be merging presorted
    // documents, and wouldn't use this method.
    std::tie(sortKey, docForSorter) = extractSortKey(std::move(doc));
    _sortExecutor->add(sortKey, docForSorter);
}

void DocumentSourceSort::loadingDone() {
    _sortExecutor->loadingDone();
    _populated = true;
}

bool DocumentSourceSort::usedDisk() const {
    return isBoundedSortStage() ? _timeSorter->stats().spilledRanges() > 0
                                : _sortExecutor->wasDiskUsed();
}

std::pair<Value, Document> DocumentSourceSort::extractSortKey(Document&& doc) const {
    Value sortKey = _sortKeyGen->computeSortKeyFromDocument(doc);

    if (shouldSetSortKeyMetadata()) {
        // If this sort stage is part of a merged pipeline, make sure that each Document's sort key
        // gets saved with its metadata.
        MutableDocument toBeSorted(std::move(doc));
        toBeSorted.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());

        return std::make_pair(std::move(sortKey), toBeSorted.freeze());
    } else {
        return std::make_pair(std::move(sortKey), std::move(doc));
    }
}

std::pair<Date_t, Document> DocumentSourceSort::extractTime(Document&& doc) const {
    const auto& fullPath = _sortExecutor->sortPattern().back().fieldPath->fullPath();
    auto time = doc.getField(StringData{fullPath});
    uassert(6369909,
            "$_internalBoundedSort only handles BSONType::date values",
            time.getType() == BSONType::date);
    auto date = time.getDate();

    if (shouldSetSortKeyMetadata()) {
        // If this sort stage is part of a merged pipeline, make sure that each Document's sort key
        // gets saved with its metadata.
        Value sortKey = _sortKeyGen->computeSortKeyFromDocument(doc);
        MutableDocument toBeSorted(std::move(doc));
        toBeSorted.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());

        return std::make_pair(date, toBeSorted.freeze());
    }

    return std::make_pair(date, std::move(doc));
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSort::distributedPlanLogic() {
    uassert(6369906,
            "$_internalBoundedSort cannot be the first stage on the merger, because it requires "
            "almost-sorted input, which the shardsPart of a pipeline can't provide",
            !_timeSorter);

    DistributedPlanLogic split;
    split.shardsStage = this;
    split.mergeSortPattern = _sortExecutor->sortPattern()
                                 .serialize(SortPattern::SortKeySerialization::kForSortKeyMerging)
                                 .toBson();
    if (auto limit = getLimit()) {
        split.mergingStages = {DocumentSourceLimit::create(pExpCtx, *limit)};
    }
    return split;
}

bool DocumentSourceSort::canRunInParallelBeforeWriteStage(
    const OrderedPathSet& nameOfShardKeyFieldsUponEntryToStage) const {
    // This is an interesting special case. If there are no further stages which require merging the
    // streams into one, a $sort should not require it. This is only the case because the sort order
    // doesn't matter for a pipeline ending with a write stage. We may encounter it here as an
    // intermediate stage before a final $group with a $sort, which would make sense. Should we
    // extend our analysis to detect if an exchange is appropriate in a general pipeline, a $sort
    // would generally require merging the streams before producing output.
    return false;
}

void DocumentSourceSort::doForceSpill() {
    if (_sortExecutor) {
        _sortExecutor->forceSpill();
    }
    if (_timeSorter) {
        _timeSorter->forceSpill();
    }
}

void DocumentSourceSort::updateTimeSorterStats() {
    tassert(10321900,
            "Called updateTimeSorterStats() on a non-bounded sort stage",
            isBoundedSortStage());
    _timeSorterStats.totalDataSizeBytes = _timeSorter->stats().bytesSorted();
    _timeSorterStats.memoryUsageBytes = _timeSorter->stats().memUsage();
    _timeSorterStats.keysSorted = _timeSorter->stats().numSorted();
    auto sorterFileStats = getSorterFileStats();
    if (sorterFileStats) {
        _timeSorterStats.spillingStats.setSpills(_timeSorter->stats().spilledRanges());
        _timeSorterStats.spillingStats.setSpilledRecords(
            _timeSorter->stats().spilledKeyValuePairs());
        _timeSorterStats.spillingStats.setSpilledBytes(sorterFileStats->bytesSpilledUncompressed());
        _timeSorterStats.spillingStats.updateSpilledDataStorageSize(
            sorterFileStats->bytesSpilled());
    }
}

}  // namespace mongo
