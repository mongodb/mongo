/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace document_source_densify {
// TODO SERVER-57334 Translation logic goes here.
}

class DocumentSourceInternalDensify final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalDensify"_sd;

    using DensifyValueType = stdx::variant<double, Date_t>;
    struct StepSpec {
        double step;
        boost::optional<TimeUnit> unit;
        boost::optional<TimeZone> tz;
    };
    class DocGenerator {
    public:
        DocGenerator(DensifyValueType min,
                     DensifyValueType max,
                     StepSpec step,
                     FieldPath fieldName,
                     boost::optional<Document> includeFields,
                     boost::optional<Document> finalDoc);
        Document getNextDocument();
        bool done() const;

    private:
        StepSpec _step;
        // The field to add to 'includeFields' to generate a document.
        FieldPath _path;
        Document _includeFields;
        // The document that is equal to or larger than '_max' that prompted the creation of this
        // generator. Will be returned after the final generated document. Can be boost::none if we
        // are generating the values at the end of the range.
        boost::optional<Document> _finalDoc;
        // The minimum value that this generator will create, therefore the next generated document
        // will have this value.
        DensifyValueType _min;
        // The maximum value that will be generated. This value is inclusive.
        DensifyValueType _max;

        enum class GeneratorState {
            // Generating documents between '_min' and '_max'.
            kGeneratingDocuments,
            // Generated all necessary documents, waiting for a final 'getNextDocument()' call.
            kReturningFinalDocument,
            kDone,
        };

        GeneratorState _state = GeneratorState::kGeneratingDocuments;
    };

    DocumentSourceInternalDensify(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        double step,
        FieldPath path,
        boost::optional<std::pair<DensifyValueType, DensifyValueType>> range = boost::none);

    static boost::intrusive_ptr<DocumentSourceInternalDensify> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // TODO SERVER-57334
        return Value(DOC("$densify" << Document()));
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    GetNextResult doGetNext() final;

private:
    enum class ValComparedToRange {
        kBelow,
        kRangeMin,
        kInside,
        kAbove,
    };

    /**
       Decides whether or not to build a DocGen and return the first document generated
       or return the current doc if the rangeMin + step is greater than rangeMax.
    */
    DocumentSource::GetNextResult handleNeedGenFull(Document currentDoc);

    /**
        Checks where the current doc's value lies compared to the range and
        creates the correct DocGen if needed and returns the next doc.
    */
    DocumentSource::GetNextResult handleNeedGenExplicit(Document currentDoc, double val);

    /** Takes care of when an EOF has been hit for the explicit case.
    It checks if we have finished densifying over the range, and if so changes the
    state to be kDensify done. Otherwise it builds a new generator that will finish
    densifying over the range and changes the state to kHaveGen. */
    DocumentSource::GetNextResult densifyAfterEOF();

    /** Creates a document generator based on the value passed in, the current
    _rangeMin, and the _rangeMax. Once created, the state changes to kHaveGenerator and the first
    document from the generator is returned. */
    DocumentSource::GetNextResult processDocAboveMinBound(double val, Document doc);

    /** Takes in a value and checks if the value is below, on the bottom, inside, or above the
    range, and returns the equivelant state from ValComparedToRange. */
    ValComparedToRange processRangeNum(double val, double rangeMin, double rangeMax);

    /** Handles when the pSource has been exhausted. In the full
    case we are done with the densification process and the state becomes kDensifyDone, however in
    the explicit case we may still need to densify over the remainder of the range, so the
    densifyAfterEOF() function is called. */
    DocumentSource::GetNextResult handleSourceExhausted();

    /** Checks if the current document generator is done. If it is and we have finished densifying,
    it changes the state to be kDensifyDone. If there is more to densify, the state becomes
    kNeedGen. The generator is also deleted. */
    void resetDocGen();

    auto valOffsetFromStep(double val, double sub, double step) {
        return fmod((val - sub), step);
    }
    boost::optional<DocGenerator> _docGenerator = boost::none;

    // The minimum value that the document generator will create, therefore the next generated
    // document will have this value.
    // This is also used as last seen value by the explicit case.
    boost::optional<DensifyValueType> _rangeMin = boost::none;
    boost::optional<DensifyValueType> _rangeMax = boost::none;

    bool _eof = false;

    enum class DensifyState { kUninitializedOrBelowRange, kNeedGen, kHaveGenerator, kDensifyDone };

    enum class TypeOfDensify {
        kFull,
        kExplicitRange,
        kPartition,
    };

    StepSpec _step;
    FieldPath _path;

    DensifyState _densifyState = DensifyState::kUninitializedOrBelowRange;
    TypeOfDensify _densifyType;
};
}  // namespace mongo
