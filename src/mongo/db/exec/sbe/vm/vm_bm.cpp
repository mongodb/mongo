/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/query/collation/collator_factory_icu.h"

namespace mongo::sbe {
namespace {

using TagValue = std::pair<value::TypeTags, value::Value>;

class ValueVectorGuard {
public:
    ValueVectorGuard(std::vector<TagValue>& values) : _values(values) {}
    ~ValueVectorGuard() {
        for (auto [tag, value] : _values) {
            value::releaseValue(tag, value);
        }
    }

private:
    std::vector<TagValue>& _values;
};

class SbeVmBenchmark : public benchmark::Fixture {
private:
    static constexpr int32_t kSeed = 1;

public:
    SbeVmBenchmark() : SbeVmBenchmark(std::make_unique<RuntimeEnvironment>()) {}

    void benchmarkExpression(std::unique_ptr<EExpression> expr,
                             const std::vector<TagValue>& inputs,
                             benchmark::State& state) {
        vm::CodeFragment code = expr->compileDirect(_compileCtx);
        vm::ByteCode vm;
        auto inputAccessor = _env->getAccessor(_inputSlotId);
        for (auto keepRunning : state) {
            for (auto [inputTag, inputVal] : inputs) {
                inputAccessor->reset(false, inputTag, inputVal);
                auto [owned, tag, val] = vm.run(&code);
                if (owned) {
                    value::releaseValue(tag, val);
                }
            }
            benchmark::ClobberMemory();
        }
    }

    TagValue generateRandomString(size_t size) {
        static const std::string kAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::string str;
        str.reserve(size);
        for (size_t j = 0; j < size; ++j) {
            str.push_back(kAlphabet[_random.nextInt32(kAlphabet.size())]);
        }
        return value::makeNewString(str);
    }

    std::vector<TagValue> generateRandomStrings(size_t count, size_t size) {
        std::vector<TagValue> strings;
        strings.reserve(count);
        for (size_t i = 0; i < count; i++) {
            strings.push_back(generateRandomString(size));
        }
        return strings;
    }

    TagValue makeArraySet(const std::vector<TagValue>& values, const CollatorInterface* collator) {
        auto [tag, value] = value::makeNewArraySet(collator);
        auto* arraySet = value::getArraySetView(value);
        for (const auto& [tag, value] : values) {
            auto [tagCopy, valueCopy] = value::copyValue(tag, value);
            arraySet->push_back(tagCopy, valueCopy);
        }
        return {tag, value};
    }

    value::SlotId setCollator(const CollatorInterface* collator) {
        auto collatorSlot = _env->getSlotIfExists("collator"_sd);
        if (collatorSlot) {
            _env->getAccessor(*collatorSlot)
                ->reset(false,
                        value::TypeTags::collator,
                        value::bitcastFrom<const CollatorInterface*>(collator));
            return *collatorSlot;
        }
        return _env->registerSlot("collator"_sd,
                                  value::TypeTags::collator,
                                  value::bitcastFrom<const CollatorInterface*>(collator),
                                  false,
                                  &_slotIdGenerator);
    }

    std::unique_ptr<CollatorInterface> createCollator() {
        auto statusWithCollator = _collatorFactory.makeFromBSON(BSON("locale"
                                                                     << "en_US"));
        invariant(statusWithCollator.isOK());
        return std::move(statusWithCollator.getValue());
    }

    value::SlotId inputSlotId() const {
        return _inputSlotId;
    }

    PseudoRandom random() const {
        return _random;
    }

private:
    SbeVmBenchmark(std::unique_ptr<RuntimeEnvironment> env)
        : _env(env.get()), _compileCtx(std::move(env)), _random(kSeed) {
        _env->registerSlot("timeZoneDB"_sd,
                           value::TypeTags::timeZoneDB,
                           value::bitcastFrom<TimeZoneDatabase*>(&_timeZoneDB),
                           false,
                           &_slotIdGenerator);
        _inputSlotId =
            _env->registerSlot("input"_sd, value::TypeTags::Nothing, 0, false, &_slotIdGenerator);
    }

    RuntimeEnvironment* _env;
    CompileCtx _compileCtx;
    value::SlotIdGenerator _slotIdGenerator;
    value::SlotId _inputSlotId;

    PseudoRandom _random;

    TimeZoneDatabase _timeZoneDB;

    CollatorFactoryICU _collatorFactory;
};

BENCHMARK_DEFINE_F(SbeVmBenchmark, BM_IsMember_ArraySet_NoCollator)(benchmark::State& state) {
    auto strings = generateRandomStrings(state.range(0) /*count*/, state.range(1) /*size*/);
    ValueVectorGuard guards(strings);

    TagValue arraySet = makeArraySet(strings, nullptr /*collator*/);
    auto arraySetConstant = makeE<EConstant>(arraySet.first, arraySet.second);

    auto expr = makeE<EFunction>(
        "isMember"_sd, makeEs(makeE<EVariable>(inputSlotId()), std::move(arraySetConstant)));
    TagValue searchValue = generateRandomString(state.range(1) /*size*/);
    value::ValueGuard guard{searchValue.first, searchValue.second};
    benchmarkExpression(std::move(expr), {searchValue}, state);
}

BENCHMARK_DEFINE_F(SbeVmBenchmark, BM_IsMember_ArraySet_Collator)(benchmark::State& state) {
    auto strings = generateRandomStrings(state.range(0) /*count*/, state.range(1) /*size*/);
    ValueVectorGuard guards(strings);
    auto collator = createCollator();
    auto collatorSlotId = setCollator(collator.get());

    TagValue arraySet = makeArraySet(strings, collator.get());
    auto arraySetConstant = makeE<EConstant>(arraySet.first, arraySet.second);

    auto expr = makeE<EFunction>("collIsMember"_sd,
                                 makeEs(makeE<EVariable>(collatorSlotId),
                                        makeE<EVariable>(inputSlotId()),
                                        std::move(arraySetConstant)));
    TagValue searchValue = generateRandomString(state.range(1) /*size*/);
    value::ValueGuard guard{searchValue.first, searchValue.second};
    benchmarkExpression(std::move(expr), {searchValue}, state);
}

BENCHMARK_DEFINE_F(SbeVmBenchmark, BM_IsMember_ArraySet_Collator_Linear)(benchmark::State& state) {
    auto strings = generateRandomStrings(state.range(0) /*count*/, state.range(1) /*size*/);
    ValueVectorGuard guards(strings);
    auto collator = createCollator();
    auto collatorSlotId = setCollator(collator.get());

    // Do not pass collator to ArraySet to make VM use linear search.
    TagValue arraySet = makeArraySet(strings, nullptr);
    auto arraySetConstant = makeE<EConstant>(arraySet.first, arraySet.second);

    auto expr = makeE<EFunction>("collIsMember"_sd,
                                 makeEs(makeE<EVariable>(collatorSlotId),
                                        makeE<EVariable>(inputSlotId()),
                                        std::move(arraySetConstant)));

    TagValue searchValue = generateRandomString(state.range(1) /*size*/);
    value::ValueGuard guard{searchValue.first, searchValue.second};
    benchmarkExpression(std::move(expr), {searchValue}, state);
}

#define ADD_ARGS()        \
    Args({5, 5})          \
        ->Args({10, 5})   \
        ->Args({10, 10})  \
        ->Args({50, 10})  \
        ->Args({10, 50})  \
        ->Args({50, 50})  \
        ->Args({10, 100}) \
        ->Args({50, 100}) \
        ->Args({100, 100})

BENCHMARK_REGISTER_F(SbeVmBenchmark, BM_IsMember_ArraySet_NoCollator)->Args({100, 100});

BENCHMARK_REGISTER_F(SbeVmBenchmark, BM_IsMember_ArraySet_Collator)->ADD_ARGS();

BENCHMARK_REGISTER_F(SbeVmBenchmark, BM_IsMember_ArraySet_Collator_Linear)->ADD_ARGS();

}  // namespace
}  // namespace mongo::sbe
