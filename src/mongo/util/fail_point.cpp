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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(dummy);  // used by tests in jstests/fail_point

MONGO_INITIALIZER_GENERAL(AllFailPointsRegistered, (), ())
(InitializerContext* context) {
    globalFailPointRegistry().freeze();
}

/** The per-thread PRNG used by fail-points. */
thread_local PseudoRandom threadPrng{SecureRandom().nextInt64()};

template <typename Pred>
void spinWait(const Pred& pred) {
    for (int n = 0; n < 100; ++n) {
        if (pred())
            return;
    }
    for (int n = 0; n < 100; ++n) {
        if (pred())
            return;
        stdx::this_thread::yield();
    }
    while (true) {
        if (pred())
            return;
        stdx::this_thread::sleep_for(stdx::chrono::milliseconds(50));
    }
}


}  // namespace

void FailPoint::setThreadPRNGSeed(int32_t seed) {
    threadPrng = PseudoRandom(seed);
}

FailPoint::FailPoint(std::string name, bool immortal) : _immortal(immortal) {
    new (_rawImpl()) Impl(std::move(name));
    _ready.store(true);
}

FailPoint::~FailPoint() {
    if (!_immortal)
        _rawImpl()->~Impl();
}

auto FailPoint::Impl::setMode(Mode mode, ValType val, BSONObj extra) -> EntryCountT {
    /**
     * Outline:
     *
     * 1. Deactivates fail point to enter write-only mode
     * 2. Waits for all current readers of the fail point to finish
     * 3. Sets the new mode.
     */

    stdx::lock_guard scoped(_modMutex);

    // Step 1
    _disable();

    // Step 2
    spinWait([&] { return _fpInfo.load() == 0; });

    // Step 3
    _mode = mode;
    _modeValue.store(val);
    _data = std::move(extra);

    if (_mode != off) {
        _enable();
    }

    return _hitCount.load();
}

auto FailPoint::Impl::waitForTimesEntered(Interruptible* interruptible,
                                          EntryCountT targetTimesEntered) const -> EntryCountT {
    while (true) {
        if (auto entries = _hitCount.load(); entries >= targetTimesEntered)
            return entries;
        interruptible->sleepFor(_kWaitGranularity);
    }
}

bool FailPoint::Impl::_evaluateByMode() {
    switch (_mode) {
        case alwaysOn:
            return true;
        case random:
            return std::uniform_int_distribution<int>{}(threadPrng.urbg()) < _modeValue.load();
        case nTimes:
            if (_modeValue.subtractAndFetch(1) <= 0)
                _disable();
            return true;
        case skip:
            // Ensure that once the skip counter reaches within some delta from 0 we don't continue
            // decrementing it unboundedly because at some point it will roll over and become
            // positive again
            return _modeValue.load() <= 0 || _modeValue.subtractAndFetch(1) < 0;
        default:
            LOGV2_ERROR(23832,
                        "FailPoint mode not supported: {mode}",
                        "FailPoint mode not supported",
                        "mode"_attr = static_cast<int>(_mode));
            fassertFailed(16444);
    }
}

StatusWith<FailPoint::ModeOptions> FailPoint::parseBSON(const BSONObj& obj) {
    Mode mode = FailPoint::alwaysOn;
    ValType val = 0;
    const BSONElement modeElem(obj["mode"]);
    if (modeElem.eoo()) {
        return {ErrorCodes::IllegalOperation, "When setting a failpoint, you must supply a 'mode'"};
    } else if (modeElem.type() == String) {
        const std::string modeStr(modeElem.str());
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

BSONObj FailPoint::Impl::toBSON() const {
    BSONObjBuilder builder;

    stdx::lock_guard scoped(_modMutex);
    builder.append("mode", _mode);
    builder.append("data", _data);
    builder.append("timesEntered", _hitCount.load());

    return builder.obj();
}

FailPointRegisterer::FailPointRegisterer(FailPoint* fp) {
    uassertStatusOK(globalFailPointRegistry().add(fp));
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

FailPointEnableBlock::FailPointEnableBlock(StringData failPointName)
    : FailPointEnableBlock(failPointName, {}) {}

FailPointEnableBlock::FailPointEnableBlock(StringData failPointName, BSONObj data)
    : FailPointEnableBlock(globalFailPointRegistry().find(failPointName), std::move(data)) {}

FailPointEnableBlock::FailPointEnableBlock(FailPoint* failPoint)
    : FailPointEnableBlock(failPoint, {}) {}

FailPointEnableBlock::FailPointEnableBlock(FailPoint* failPoint, BSONObj data)
    : _failPoint(failPoint) {
    invariant(_failPoint != nullptr);

    _initialTimesEntered = _failPoint->setMode(FailPoint::alwaysOn, 0, std::move(data));

    LOGV2_WARNING(23830,
                  "Set failpoint {failPointName} to: {failPoint}",
                  "Set failpoint",
                  "failPointName"_attr = _failPoint->getName(),
                  "failPoint"_attr = _failPoint->toBSON());
}

FailPointEnableBlock::~FailPointEnableBlock() {
    _failPoint->setMode(FailPoint::off);
    LOGV2_WARNING(23831,
                  "Set failpoint {failPointName} to: {failPoint}",
                  "Set failpoint",
                  "failPointName"_attr = _failPoint->getName(),
                  "failPoint"_attr = _failPoint->toBSON());
}

FailPointRegistry::FailPointRegistry() : _frozen(false) {}

Status FailPointRegistry::add(FailPoint* failPoint) {
    if (_frozen) {
        return {ErrorCodes::CannotMutateObject, "Registry is already frozen"};
    }
    auto [pos, ok] = _fpMap.insert({failPoint->getName(), failPoint});
    if (!ok) {
        return {ErrorCodes::Error(51006),
                "Fail point already registered: {}"_format(failPoint->getName())};
    }
    return Status::OK();
}

FailPoint* FailPointRegistry::find(StringData name) const {
    auto iter = _fpMap.find(name);
    return (iter == _fpMap.end()) ? nullptr : iter->second;
}

void FailPointRegistry::freeze() {
    _frozen = true;
}

void FailPointRegistry::registerAllFailPointsAsServerParameters() {
    for (const auto& [name, ptr] : _fpMap) {
        makeServerParameter<FailPointServerParameter>(name, ServerParameterType::kStartupOnly);
    }
}

void FailPointRegistry::disableAllFailpoints() {
    for (auto& [_, fp] : _fpMap) {
        fp->setMode(FailPoint::Mode::off);
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
                                      BSONObjBuilder* b,
                                      StringData name,
                                      const boost::optional<TenantId>&) {
    *b << name << _data->toBSON();
}

Status FailPointServerParameter::setFromString(StringData str, const boost::optional<TenantId>&) {
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
