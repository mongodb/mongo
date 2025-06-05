/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/ftdc/metadata_compressor.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

namespace {
/*
 * Returns whether the sample object has the same sequence of field names as the ref object.
 */
bool haveSameFields(BSONObj ref, BSONObj sample) {
    bool match = true;
    BSONObjIterator refItr(ref);
    BSONObjIterator sampleItr(sample);
    while (match && refItr.more() && sampleItr.more()) {
        match &= (refItr.next().fieldNameStringData() == sampleItr.next().fieldNameStringData());
    }
    match &= !(sampleItr.more() || refItr.more());
    return match;
};

/*
 * Manages a stack of BSONObjBuilders, each being a subobject builder for the previous builder.
 * The actual creation of the topmost builder (and perhaps its parent builders) is deferred until
 * a BSONElement is appended to it.
 */
class JITBSONObjBuilderStack {
    JITBSONObjBuilderStack(const JITBSONObjBuilderStack&) = delete;
    JITBSONObjBuilderStack& operator=(const JITBSONObjBuilderStack&) = delete;

public:
    JITBSONObjBuilderStack(BSONObjBuilder* root) : _root(root) {}

    ~JITBSONObjBuilderStack() {
        while (!_stack.empty()) {
            _stack.pop_back();
        }
    }

    void push(StringData sd) {
        _stack.emplace_back(sd);
    }

    void append(const BSONElement& elt) {
        activateBuilders().append(elt);
    }

    // If the builder at the current level is not yet created, this stashes the given element
    // until the subobject builder at that level is created, during which stashed elements are
    // appended to it in the order they were stashed.
    void stashOrAppend(const BSONElement& elt) {
        if (_stack.empty()) {
            _root->append(elt);
        } else if (_stack.back().builder) {
            _stack.back().builder->append(elt);
        } else {
            _stack.back().stash.push_back(elt);
        }
    }

    void pop() {
        invariant(!_stack.empty());
        _stack.pop_back();
    }

    size_t depth() const {
        return _stack.size();
    }

private:
    BSONObjBuilder& activateBuilders() {
        if (!_stack.empty() && _stack.back().builder) {
            return *(_stack.back().builder);
        }

        BSONObjBuilder* prevBuilder = _root;
        for (auto& frame : _stack) {
            if (!frame.builder) {
                frame.builder =
                    std::make_unique<BSONObjBuilder>(prevBuilder->subobjStart(frame.name));
                for (auto& element : frame.stash) {
                    frame.builder->append(element);
                }
                frame.stash.clear();
            }
            prevBuilder = frame.builder.get();
        }
        return *prevBuilder;
    }

    struct JITBSONObjBuilderStackFrame {
        JITBSONObjBuilderStackFrame(StringData subObjectName) : name(subObjectName) {};
        StringData name;
        std::unique_ptr<BSONObjBuilder> builder{nullptr};
        std::vector<BSONElement> stash;
    };
    std::vector<JITBSONObjBuilderStackFrame> _stack;
    BSONObjBuilder* _root;
};

/*
 * Compares the elements of reference & sample and appends the delta to builder.
 * Returns whether changes were detected in fields other than the "start" & "end" fields, or
 * boost::none if there was a schema change.
 */
boost::optional<bool> compareAndBuildDeltaFinal(BSONObj reference,
                                                BSONObj sample,
                                                JITBSONObjBuilderStack* builder) {
    if (!haveSameFields(reference, sample)) {
        return boost::none;
    }

    // Holds any elements that don't need to appear if no substantial changes were found
    bool hasChanges = false;

    BSONObjIterator sampleItr(sample);
    BSONObjIterator refItr(reference);

    // reference and sample are the same size, as established in haveSameFields above
    while (refItr.more()) {
        auto refElement = refItr.next();
        auto sampleElement = sampleItr.next();
        auto fieldName = sampleElement.fieldNameStringData();

        if (fieldName == "start"_sd || fieldName == "end"_sd) {
            dassert(sampleElement.type() == BSONType::date);
            builder->stashOrAppend(sampleElement);
            continue;
        }

        if (sampleElement.woCompare(refElement) != 0) {
            hasChanges = true;
            builder->append(sampleElement);
        }
    }
    return hasChanges;
}

/*
 * Recursively builds the delta document between the reference & sample documents,
 * up to the depth specified in maxDepth.
 * Returns whether changes were detected between the reference and sample, or boost::none if
 * there was a schema change.
 */
boost::optional<bool> compareAndBuildDelta(BSONObj reference,
                                           BSONObj sample,
                                           JITBSONObjBuilderStack* builder,
                                           size_t maxDepth) {
    dassert(maxDepth > 0);

    if (!haveSameFields(reference, sample)) {
        return boost::none;
    }

    BSONObjIterator sampleItr(sample);
    BSONObjIterator refItr(reference);
    bool hasChanges = false;

    // reference and sample are the same size, as established in haveSameFields above
    while (refItr.more()) {
        auto refElement = refItr.next();
        auto sampleElement = sampleItr.next();
        auto fieldName = sampleElement.fieldNameStringData();

        if (fieldName == "start"_sd || fieldName == "end"_sd) {
            dassert(sampleElement.type() == BSONType::date);
            builder->stashOrAppend(sampleElement);
            continue;
        }

        dassert(sampleElement.type() == BSONType::object);
        dassert(refElement.type() == BSONType::object);
        auto sampleSubObj = sampleElement.Obj();
        auto refSubObj = refElement.Obj();

        boost::optional<bool> cleanCompare;
        builder->push(fieldName);
        if (maxDepth == 1) {
            cleanCompare = compareAndBuildDeltaFinal(refSubObj, sampleSubObj, builder);
        } else {
            cleanCompare = compareAndBuildDelta(refSubObj, sampleSubObj, builder, maxDepth - 1);
        }
        builder->pop();

        if (!cleanCompare.has_value()) {
            return cleanCompare;
        }
        hasChanges |= cleanCompare.value();
    }

    return hasChanges;
}
}  // namespace

/*
 * Without multiServiceSchema, this expects the input sample document to have the following layout:
 * {
 *   start: Date_t,
 *   cmdName: {
 *      start: Date_t,
 *      replyField: Value,
 *      ...
 *      end: Date_t,
 *   },
 *   ...,
 *   end: Date_t,
 * }
 * where cmdName & replyField are placeholders for a command name and a field in the command
 * response, respectively.
 *
 * With multiServiceSchema enabled, this expects the input sample document to have the following
 * layout:
 * {
 *   start: Date_t,
 *   roleName: {
 *      cmdName: {
 *         start: Date_t,
 *         replyField: Value,
 *         ...
 *         end: Date_t,
 *      },
 *      ...
 *   },
 *   ...,
 *   end: Date_t,
 * }
 * where roleName is either "shard", "router", or "common".
 */
boost::optional<BSONObj> FTDCMetadataCompressor::addSample(const BSONObj& sample) {
    BSONObjBuilder deltaDocBuilder;
    JITBSONObjBuilderStack jitBuilderStack(&deltaDocBuilder);

    auto cleanCompare = compareAndBuildDelta(
        _referenceDoc, sample, &jitBuilderStack, _multiServiceSchema ? 2 : 1 /*maxDepth*/);
    if (!cleanCompare.has_value()) {
        _reset(sample);
        return sample;
    }

    dassert(jitBuilderStack.depth() == 0);

    if (cleanCompare.value()) {
        _referenceDoc = sample;
        _deltaCount++;
        return deltaDocBuilder.obj();
    }

    return boost::none;
}

void FTDCMetadataCompressor::reset() {
    _reset(BSONObj());
}

void FTDCMetadataCompressor::_reset(const BSONObj& newReference) {
    _deltaCount = 0;
    _referenceDoc = newReference;
}

}  // namespace mongo
