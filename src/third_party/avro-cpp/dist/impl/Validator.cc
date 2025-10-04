/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utility>

#include "NodeImpl.hh"
#include "ValidSchema.hh"
#include "Validator.hh"

namespace avro {

Validator::Validator(ValidSchema schema) : schema_(std::move(schema)),
                                           nextType_(AVRO_NULL),
                                           expectedTypesFlag_(0),
                                           compoundStarted_(false),
                                           waitingForCount_(false),
                                           count_(0) {
    setupOperation(schema_.root());
}

void Validator::setWaitingForCount() {
    waitingForCount_ = true;
    count_ = 0;
    expectedTypesFlag_ = typeToFlag(AVRO_INT) | typeToFlag(AVRO_LONG);
    nextType_ = AVRO_LONG;
}

void Validator::enumAdvance() {
    if (compoundStarted_) {
        setWaitingForCount();
        compoundStarted_ = false;
    } else {
        waitingForCount_ = false;
        compoundStack_.pop_back();
    }
}

bool Validator::countingSetup() {
    auto proceed = true;
    if (compoundStarted_) {
        setWaitingForCount();
        compoundStarted_ = false;
        proceed = false;
    } else if (waitingForCount_) {
        waitingForCount_ = false;
        if (count_ == 0) {
            compoundStack_.pop_back();
            proceed = false;
        } else {
            counters_.push_back(count_);
        }
    }

    return proceed;
}

void Validator::countingAdvance() {
    if (countingSetup()) {
        size_t index = (compoundStack_.back().pos)++;
        const NodePtr &node = compoundStack_.back().node;

        if (index < node->leaves()) {
            setupOperation(node->leafAt(index));
        } else {
            compoundStack_.back().pos = 0;
            size_t count = --counters_.back();
            if (count == 0) {
                counters_.pop_back();
                compoundStarted_ = true;
                nextType_ = node->type();
                expectedTypesFlag_ = typeToFlag(nextType_);
            } else {
                index = (compoundStack_.back().pos)++;
                setupOperation(node->leafAt(index));
            }
        }
    }
}

void Validator::unionAdvance() {
    if (compoundStarted_) {
        setWaitingForCount();
        compoundStarted_ = false;
    } else {
        waitingForCount_ = false;
        NodePtr node = compoundStack_.back().node;

        if (count_ < node->leaves()) {
            compoundStack_.pop_back();
            setupOperation(node->leafAt(static_cast<int>(count_)));
        } else {
            throw Exception(
                "Union selection out of range, got {}, expecting 0-{}",
                count_, node->leaves() - 1);
        }
    }
}

void Validator::fixedAdvance() {
    compoundStarted_ = false;
    compoundStack_.pop_back();
}

size_t Validator::nextSizeExpected() const {
    return compoundStack_.back().node->fixedSize();
}

void Validator::doAdvance() {
    using AdvanceFunc = void (Validator::*)();

    // only the compound types need advance functions here
    static const AdvanceFunc funcs[] = {
        nullptr,                     // string
        nullptr,                     // bytes
        nullptr,                     // int
        nullptr,                     // long
        nullptr,                     // float
        nullptr,                     // double
        nullptr,                     // bool
        nullptr,                     // null
        &Validator::countingAdvance, // Record is treated like counting with count == 1
        &Validator::enumAdvance,
        &Validator::countingAdvance,
        &Validator::countingAdvance,
        &Validator::unionAdvance,
        &Validator::fixedAdvance};
    static_assert((sizeof(funcs) / sizeof(AdvanceFunc)) == (AVRO_NUM_TYPES),
                  "Invalid number of advance functions");

    expectedTypesFlag_ = 0;
    // loop until we encounter a next expected type, or we've exited all compound types
    while (!expectedTypesFlag_ && !compoundStack_.empty()) {

        Type type = compoundStack_.back().node->type();

        AdvanceFunc func = funcs[type];

        // only compound functions are put on the status stack so it is ok to
        // assume that func is not null
        assert(func);

        ((this)->*(func))();
    }

    if (compoundStack_.empty()) {
        nextType_ = AVRO_NULL;
    }
}

void Validator::advance() {
    if (!waitingForCount_) {
        doAdvance();
    }
}

void Validator::setCount(size_t count) {
    if (!waitingForCount_) {
        throw Exception("Not expecting count");
    }
    count_ = count;

    doAdvance();
}

void Validator::setupFlag(Type type) {
    // use flags instead of strictly types, so that we can be more lax about the type
    // (for example, a long should be able to accept an int type, but not vice versa)
    static const flag_t flags[] = {
        typeToFlag(AVRO_STRING) | typeToFlag(AVRO_BYTES),
        typeToFlag(AVRO_STRING) | typeToFlag(AVRO_BYTES),
        typeToFlag(AVRO_INT),
        typeToFlag(AVRO_INT) | typeToFlag(AVRO_LONG),
        typeToFlag(AVRO_FLOAT),
        typeToFlag(AVRO_DOUBLE),
        typeToFlag(AVRO_BOOL),
        typeToFlag(AVRO_NULL),
        typeToFlag(AVRO_RECORD),
        typeToFlag(AVRO_ENUM),
        typeToFlag(AVRO_ARRAY),
        typeToFlag(AVRO_MAP),
        typeToFlag(AVRO_UNION),
        typeToFlag(AVRO_FIXED)};
    static_assert((sizeof(flags) / sizeof(flag_t)) == (AVRO_NUM_TYPES),
                  "Invalid number of avro type flags");

    expectedTypesFlag_ = flags[type];
}

void Validator::setupOperation(const NodePtr &node) {
    nextType_ = node->type();

    if (nextType_ == AVRO_SYMBOLIC) {
        NodePtr actualNode = resolveSymbol(node);
        assert(actualNode);
        setupOperation(actualNode);
        return;
    }

    assert(nextType_ < AVRO_SYMBOLIC);

    setupFlag(nextType_);

    if (!isPrimitive(nextType_)) {
        compoundStack_.emplace_back(node);
        compoundStarted_ = true;
    }
}

bool Validator::getCurrentRecordName(std::string &name) const {
    auto found = false;
    name.clear();

    // if the top of the stack is a record I want this record name
    auto idx = static_cast<int>(compoundStack_.size() - ((!compoundStack_.empty() && (isPrimitive(nextType_) || nextType_ == AVRO_RECORD)) ? 1 : 2));

    if (idx >= 0 && compoundStack_[idx].node->type() == AVRO_RECORD) {
        name = compoundStack_[idx].node->name().simpleName();
        found = true;
    }
    return found;
}

bool Validator::getNextFieldName(std::string &name) const {
    auto found = false;
    name.clear();
    auto idx = static_cast<int>(compoundStack_.size() - (isCompound(nextType_) ? 2 : 1));
    if (idx >= 0 && compoundStack_[idx].node->type() == AVRO_RECORD) {
        size_t pos = compoundStack_[idx].pos - 1;
        const NodePtr &node = compoundStack_[idx].node;
        if (pos < node->leaves()) {
            name = node->nameAt(pos);
            found = true;
        }
    }
    return found;
}

} // namespace avro
