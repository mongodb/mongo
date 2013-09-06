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

#include "mongo/util/fail_point.h"

#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

using mongoutils::str::stream;

namespace mongo {
    FailPoint::FailPoint():
            _fpInfo(0),
            _mode(off),
            _timesOrPeriod(0),
            _modMutex("failPointMutex") {
    }

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

        scoped_lock scoped(_modMutex);

        // Step 1
        disableFailPoint();

        // Step 2
        while (_fpInfo.load() != 0) {
            sleepmillis(50);
        }

        // Step 3
        uassert(16442, stream() << "mode not supported " << static_cast<int>(mode),
                mode >= off && mode < numModes);

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

    FailPoint::RetCode FailPoint::slowShouldFailOpenBlock() {
        ValType localFpInfo = _fpInfo.addAndFetch(1);

        if ((localFpInfo & ACTIVE_BIT) == 0) {
            return slowOff;
        }

        switch (_mode) {
        case alwaysOn:
            return slowOn;

        case random:
            // TODO: randomly determine if should be active or not
            error() << "FailPoint Mode random is not yet supported." << endl;
            fassertFailed(16443);

        case nTimes:
        {
            AtomicInt32::WordType newVal = _timesOrPeriod.subtractAndFetch(1);

            if (newVal <= 0) {
                disableFailPoint();
            }

            return slowOn;
        }

        default:
            error() << "FailPoint Mode not supported: " << static_cast<int>(_mode) << endl;
            fassertFailed(16444);
        }
    }

    BSONObj FailPoint::toBSON() const {
        BSONObjBuilder builder;

        scoped_lock scoped(_modMutex);
        builder.append("mode", _mode);
        builder.append("data", _data);

        return builder.obj();
    }

    ScopedFailPoint::ScopedFailPoint(FailPoint* failPoint):
            _failPoint(failPoint),
            _once(false),
            _shouldClose(false) {
    }

    ScopedFailPoint::~ScopedFailPoint() {
        if (_shouldClose) {
            _failPoint->shouldFailCloseBlock();
        }
    }

    const BSONObj& ScopedFailPoint::getData() const {
        // Assert when attempting to get data without incrementing ref counter.
        fassert(16445, _shouldClose);
        return _failPoint->getData();
    }
}
