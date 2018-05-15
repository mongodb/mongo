/*
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/util/fail_point.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

/**
 * Type representing the per-thread PRNG used by fail-points.
 */
class FailPointPRNG {
public:
    FailPointPRNG() : _prng(std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64()) {}

    void resetSeed(int32_t seed) {
        _prng = PseudoRandom(seed);
    }

    int32_t nextPositiveInt32() {
        return _prng.nextInt32() & ~(1 << 31);
    }

    static FailPointPRNG* current() {
        if (!_failPointPrng)
            _failPointPrng = stdx::make_unique<FailPointPRNG>();
        return _failPointPrng.get();
    }

private:
    PseudoRandom _prng;
    static thread_local std::unique_ptr<FailPointPRNG> _failPointPrng;
};

thread_local std::unique_ptr<FailPointPRNG> FailPointPRNG::_failPointPrng;

}  // namespace

void FailPoint::setThreadPRNGSeed(int32_t seed) {
    FailPointPRNG::current()->resetSeed(seed);
}

FailPoint::FailPoint() = default;

void FailPoint::shouldFailCloseBlock() {
    _fpInfo.subtractAndFetch(1);
}

void FailPoint::setMode(Mode mode, ValType val, const BSONObj& extra) {
    /**
     * Outline:
     *
     * 1. Deactivates fail point to enter write-only mode
     * 2. Waits for all current readers of the fail point to finish
     * 3. Sets the new mode.
     */

    stdx::lock_guard<stdx::mutex> scoped(_modMutex);

    // Step 1
    disableFailPoint();

    // Step 2
    while (_fpInfo.load() != 0) {
        sleepmillis(50);
    }

    _mode = mode;
    _timesOrPeriod.store(val);

    _data = extra.copy();

    if (_mode != off) {
        enableFailPoint();
    }
}

const BSONObj& FailPoint::getData() const {
    return _data;
}

void FailPoint::enableFailPoint() {
    // TODO: Better to replace with a bitwise OR, once available for AU32
    ValType currentVal = _fpInfo.load();
    ValType expectedCurrentVal;
    ValType newVal;

    do {
        expectedCurrentVal = currentVal;
        newVal = expectedCurrentVal | ACTIVE_BIT;
        currentVal = _fpInfo.compareAndSwap(expectedCurrentVal, newVal);
    } while (expectedCurrentVal != currentVal);
}

void FailPoint::disableFailPoint() {
    // TODO: Better to replace with a bitwise AND, once available for AU32
    ValType currentVal = _fpInfo.load();
    ValType expectedCurrentVal;
    ValType newVal;

    do {
        expectedCurrentVal = currentVal;
        newVal = expectedCurrentVal & REF_COUNTER_MASK;
        currentVal = _fpInfo.compareAndSwap(expectedCurrentVal, newVal);
    } while (expectedCurrentVal != currentVal);
}

FailPoint::RetCode FailPoint::slowShouldFailOpenBlock(
    stdx::function<bool(const BSONObj&)> cb) noexcept {
    ValType localFpInfo = _fpInfo.addAndFetch(1);

    if ((localFpInfo & ACTIVE_BIT) == 0) {
        return slowOff;
    }

    if (cb && !cb(getData())) {
        return userIgnored;
    }

    switch (_mode) {
        case alwaysOn:
            return slowOn;
        case random: {
            const AtomicInt32::WordType maxActivationValue = _timesOrPeriod.load();
            if (FailPointPRNG::current()->nextPositiveInt32() < maxActivationValue)
                return slowOn;

            return slowOff;
        }
        case nTimes: {
            if (_timesOrPeriod.subtractAndFetch(1) <= 0)
                disableFailPoint();

            return slowOn;
        }
        case skip: {
            // Ensure that once the skip counter reaches within some delta from 0 we don't continue
            // decrementing it unboundedly because at some point it will roll over and become
            // positive again
            if (_timesOrPeriod.load() <= 0 || _timesOrPeriod.subtractAndFetch(1) < 0)
                return slowOn;

            return slowOff;
        }
        default:
            error() << "FailPoint Mode not supported: " << static_cast<int>(_mode);
            fassertFailed(16444);
    }
}

StatusWith<std::tuple<FailPoint::Mode, FailPoint::ValType, BSONObj>> FailPoint::parseBSON(
    const BSONObj& obj) {
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
            return {ErrorCodes::BadValue, str::stream() << "unknown mode: " << modeStr};
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
                        str::stream() << "activationProbability must be between 0.0 and 1.0; found "
                                      << activationProbability};
            }
            val = static_cast<int32_t>(std::numeric_limits<int32_t>::max() * activationProbability);
        } else {
            return {
                ErrorCodes::BadValue,
                "'mode' must be one of 'off', 'alwaysOn', 'times', and 'activationProbability'"};
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

    return std::make_tuple(mode, val, data);
}

BSONObj FailPoint::toBSON() const {
    BSONObjBuilder builder;

    stdx::lock_guard<stdx::mutex> scoped(_modMutex);
    builder.append("mode", _mode);
    builder.append("data", _data);

    return builder.obj();
}
}
