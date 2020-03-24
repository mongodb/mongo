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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl
#include "mongo/platform/basic.h"

#include "mongo/util/fail_point.h"

#include <fmt/format.h>

#include <limits>
#include <memory>
#include <random>

#include "mongo/base/init.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_server_parameter_gen.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(dummy);  // used by tests in jstests/fail_point

MONGO_INITIALIZER_GENERAL(AllFailPointsRegistered, (), ())
(InitializerContext* context) {
    globalFailPointRegistry().freeze();
    return Status::OK();
}

/** The per-thread PRNG used by fail-points. */
thread_local PseudoRandom threadPrng{SecureRandom().nextInt64()};

}  // namespace

void FailPoint::setThreadPRNGSeed(int32_t seed) {
    threadPrng = PseudoRandom(seed);
}

FailPoint::FailPoint() = default;

void FailPoint::_shouldFailCloseBlock() {
    _fpInfo.subtractAndFetch(1);
}

auto FailPoint::setMode(Mode mode, ValType val, BSONObj extra) -> EntryCountT {
    /**
     * Outline:
     *
     * 1. Deactivates fail point to enter write-only mode
     * 2. Waits for all current readers of the fail point to finish
     * 3. Sets the new mode.
     */

    stdx::lock_guard<Latch> scoped(_modMutex);

    // Step 1
    _disable();

    // Step 2
    while (_fpInfo.load() != 0) {
        sleepmillis(50);
    }

    // Step 3
    _mode = mode;
    _timesOrPeriod.store(val);
    _data = std::move(extra);

    if (_mode != off) {
        _enable();
    }

    return _timesEntered.load();
}

auto FailPoint::waitForTimesEntered(EntryCountT targetTimesEntered) const noexcept -> EntryCountT {
    auto timesEntered = _timesEntered.load();
    for (; timesEntered < targetTimesEntered; timesEntered = _timesEntered.load()) {
        sleepmillis(duration_cast<Milliseconds>(kWaitGranularity).count());
    };
    return timesEntered;
}

auto FailPoint::waitForTimesEntered(OperationContext* opCtx, EntryCountT targetTimesEntered) const
    -> EntryCountT {
    auto timesEntered = _timesEntered.load();
    for (; timesEntered < targetTimesEntered; timesEntered = _timesEntered.load()) {
        opCtx->sleepFor(kWaitGranularity);
    };
    return timesEntered;
}

const BSONObj& FailPoint::_getData() const {
    return _data;
}

void FailPoint::_enable() {
    _fpInfo.fetchAndBitOr(kActiveBit);
}

void FailPoint::_disable() {
    _fpInfo.fetchAndBitAnd(~kActiveBit);
}

FailPoint::RetCode FailPoint::_slowShouldFailOpenBlockWithoutIncrementingTimesEntered(
    std::function<bool(const BSONObj&)> cb) noexcept {
    ValType localFpInfo = _fpInfo.addAndFetch(1);

    if ((localFpInfo & kActiveBit) == 0) {
        return slowOff;
    }

    if (cb && !cb(_getData())) {
        return userIgnored;
    }

    switch (_mode) {
        case alwaysOn:
            return slowOn;
        case random: {
            std::uniform_int_distribution<int> distribution{};
            if (distribution(threadPrng.urbg()) < _timesOrPeriod.load()) {
                return slowOn;
            }
            return slowOff;
        }
        case nTimes: {
            if (_timesOrPeriod.subtractAndFetch(1) <= 0)
                _disable();
            return slowOn;
        }
        case skip: {
            // Ensure that once the skip counter reaches within some delta from 0 we don't continue
            // decrementing it unboundedly because at some point it will roll over and become
            // positive again
            if (_timesOrPeriod.load() <= 0 || _timesOrPeriod.subtractAndFetch(1) < 0) {
                return slowOn;
            }

            return slowOff;
        }
        default:
            LOGV2_ERROR(23832,
                        "FailPoint mode not supported: {mode}",
                        "FailPoint mode not supported",
                        "mode"_attr = static_cast<int>(_mode));
            fassertFailed(16444);
    }
}

FailPoint::RetCode FailPoint::_slowShouldFailOpenBlock(
    std::function<bool(const BSONObj&)> cb) noexcept {
    auto ret = _slowShouldFailOpenBlockWithoutIncrementingTimesEntered(cb);
    if (ret == slowOn) {
        _timesEntered.addAndFetch(1);
    }
    return ret;
}

StatusWith<FailPoint::ModeOptions> FailPoint::parseBSON(const BSONObj& obj) {
    Mode mode = FailPoint::alwaysOn;
    ValType val = 0;
    const BSONElement modeElem(obj["mode"]);
    if (modeElem.eoo()) {
        return {ErrorCodes::IllegalOperation, "When setting a failpoint, you must supply a 'mode'"};
    } else if (modeElem.type() == String) {
        const std::string modeStr(modeElem.valuestr());
        if (modeStr == "off") {
            mode = FailPoint::off;
        } else if (modeStr == "alwaysOn") {
            mode = FailPoint::alwaysOn;
        } else {
            return {ErrorCodes::BadValue, "unknown mode: {}"_format(modeStr)};
        }
    } else if (modeElem.type() == Object) {
        const BSONObj modeObj(modeElem.Obj());

        if (modeObj.hasField("times")) {
            mode = FailPoint::nTimes;

            long long longVal;
            auto status = bsonExtractIntegerField(modeObj, "times", &longVal);
            if (!status.isOK()) {
                return status;
            }

            if (longVal < 0) {
                return {ErrorCodes::BadValue, "'times' option to 'mode' must be positive"};
            }

            if (longVal > std::numeric_limits<int>::max()) {
                return {ErrorCodes::BadValue, "'times' option to 'mode' is too large"};
            }
            val = static_cast<int>(longVal);
        } else if (modeObj.hasField("skip")) {
            mode = FailPoint::skip;

            long long longVal;
            auto status = bsonExtractIntegerField(modeObj, "skip", &longVal);
            if (!status.isOK()) {
                return status;
            }

            if (longVal < 0) {
                return {ErrorCodes::BadValue, "'skip' option to 'mode' must be positive"};
            }

            if (longVal > std::numeric_limits<int>::max()) {
                return {ErrorCodes::BadValue, "'skip' option to 'mode' is too large"};
            }
            val = static_cast<int>(longVal);
        } else if (modeObj.hasField("activationProbability")) {
            mode = FailPoint::random;

            if (!modeObj["activationProbability"].isNumber()) {
                return {ErrorCodes::TypeMismatch,
                        "the 'activationProbability' option to 'mode' must be a double between 0 "
                        "and 1"};
            }

            const double activationProbability = modeObj["activationProbability"].numberDouble();
            if (activationProbability < 0 || activationProbability > 1) {
                return {ErrorCodes::BadValue,
                        "activationProbability must be between 0.0 and 1.0; "
                        "found {}"_format(activationProbability)};
            }
            val = static_cast<int32_t>(std::numeric_limits<int32_t>::max() * activationProbability);
        } else {
            return {ErrorCodes::BadValue,
                    "'mode' must be one of 'off', 'alwaysOn', '{times:n}', '{skip:n}' or "
                    "'{activationProbability:p}'"};
        }
    } else {
        return {ErrorCodes::TypeMismatch, "'mode' must be a string or JSON object"};
    }

    BSONObj data;
    if (obj.hasField("data")) {
        if (!obj["data"].isABSONObj()) {
            return {ErrorCodes::TypeMismatch, "the 'data' option must be a JSON object"};
        }
        data = obj["data"].Obj().getOwned();
    }

    return ModeOptions{mode, val, data};
}

BSONObj FailPoint::toBSON() const {
    BSONObjBuilder builder;

    stdx::lock_guard<Latch> scoped(_modMutex);
    builder.append("mode", _mode);
    builder.append("data", _data);
    builder.append("timesEntered", _timesEntered.load());

    return builder.obj();
}

FailPointRegisterer::FailPointRegisterer(const std::string& name, FailPoint* fp) {
    uassertStatusOK(globalFailPointRegistry().add(name, fp));
}

FailPointRegistry& globalFailPointRegistry() {
    static auto& p = *new FailPointRegistry();
    return p;
}

auto setGlobalFailPoint(const std::string& failPointName, const BSONObj& cmdObj)
    -> FailPoint::EntryCountT {
    FailPoint* failPoint = globalFailPointRegistry().find(failPointName);
    if (failPoint == nullptr)
        uasserted(ErrorCodes::FailPointSetFailed, failPointName + " not found");
    auto timesEntered = failPoint->setMode(uassertStatusOK(FailPoint::parseBSON(cmdObj)));
    LOGV2_WARNING(23829,
                  "Set failpoint {failPointName} to: {failPoint}",
                  "Set failpoint",
                  "failPointName"_attr = failPointName,
                  "failPoint"_attr = failPoint->toBSON());
    return timesEntered;
}

FailPointEnableBlock::FailPointEnableBlock(std::string failPointName)
    : FailPointEnableBlock(std::move(failPointName), {}) {}

FailPointEnableBlock::FailPointEnableBlock(std::string failPointName, BSONObj data)
    : _failPointName(std::move(failPointName)) {
    _failPoint = globalFailPointRegistry().find(_failPointName);
    invariant(_failPoint != nullptr);

    _initialTimesEntered = _failPoint->setMode(FailPoint::alwaysOn, 0, std::move(data));

    LOGV2_WARNING(23830,
                  "Set failpoint {failPointName} to: {failPoint}",
                  "Set failpoint",
                  "failPointName"_attr = _failPointName,
                  "failPoint"_attr = _failPoint->toBSON());
}

FailPointEnableBlock::~FailPointEnableBlock() {
    _failPoint->setMode(FailPoint::off);
    LOGV2_WARNING(23831,
                  "Set failpoint {failPointName} to: {failPoint}",
                  "Set failpoint",
                  "failPointName"_attr = _failPointName,
                  "failPoint"_attr = _failPoint->toBSON());
}

FailPointRegistry::FailPointRegistry() : _frozen(false) {}

Status FailPointRegistry::add(const std::string& name, FailPoint* failPoint) {
    if (_frozen) {
        return {ErrorCodes::CannotMutateObject, "Registry is already frozen"};
    }
    auto [pos, ok] = _fpMap.insert({name, failPoint});
    if (!ok) {
        return {ErrorCodes::Error(51006), "Fail point already registered: {}"_format(name)};
    }
    return Status::OK();
}

FailPoint* FailPointRegistry::find(const std::string& name) const {
    auto iter = _fpMap.find(name);
    return (iter == _fpMap.end()) ? nullptr : iter->second;
}

void FailPointRegistry::freeze() {
    _frozen = true;
}

void FailPointRegistry::registerAllFailPointsAsServerParameters() {
    for (const auto& [name, ptr] : _fpMap) {
        // Intentionally leaked.
        new FailPointServerParameter(name, ServerParameterType::kStartupOnly);
    }
}

static constexpr auto kFailPointServerParameterPrefix = "failpoint."_sd;

FailPointServerParameter::FailPointServerParameter(StringData name, ServerParameterType spt)
    : ServerParameter("{}{}"_format(kFailPointServerParameterPrefix, name), spt),
      _data(globalFailPointRegistry().find(name.toString())) {
    invariant(name != "failpoint.*", "Failpoint prototype was auto-registered from IDL");
    invariant(_data != nullptr, "Unknown failpoint: {}"_format(name));
}

void FailPointServerParameter::append(OperationContext* opCtx,
                                      BSONObjBuilder& b,
                                      const std::string& name) {
    b << name << _data->toBSON();
}

Status FailPointServerParameter::setFromString(const std::string& str) {
    BSONObj failPointOptions;
    try {
        failPointOptions = fromjson(str);
    } catch (DBException& ex) {
        return ex.toStatus();
    }

    auto swParsedOptions = FailPoint::parseBSON(failPointOptions);
    if (!swParsedOptions.isOK()) {
        return swParsedOptions.getStatus();
    }
    _data->setMode(std::move(swParsedOptions.getValue()));
    return Status::OK();
}

}  // namespace mongo
