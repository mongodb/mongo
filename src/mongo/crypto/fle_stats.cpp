/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_options_gen.h"
#include "mongo/crypto/fle_stats.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/testing_options_gen.h"

namespace mongo {

namespace {
FLEStatusSection fleStatusSection{};
}  // namespace

FLEStatusSection::FLEStatusSection() : FLEStatusSection(globalSystemTickSource()) {}

FLEStatusSection::FLEStatusSection(TickSource* tickSource)
    : ServerStatusSection("fle"), _tickSource(tickSource) {
    ECStats zeroStats;
    ECOCStats zeroECOC;

    _compactStats.setEsc(zeroStats);
    _compactStats.setEcoc(zeroECOC);
    _cleanupStats.setEsc(zeroStats);
    _cleanupStats.setEcoc(zeroECOC);
}

FLEStatusSection& FLEStatusSection::get() {
    return fleStatusSection;
}

BSONObj FLEStatusSection::generateSection(OperationContext* opCtx,
                                          const BSONElement& configElement) const {
    BSONObjBuilder builder;
    {
        CompactStats temp;
        {
            stdx::lock_guard<Mutex> lock(_compactMutex);
            temp = _compactStats;
        }
        auto sub = BSONObjBuilder(builder.subobjStart("compactStats"));
        temp.serialize(&sub);
    }
    {
        CleanupStats temp;
        {
            stdx::lock_guard<Mutex> lock(_cleanupMutex);
            temp = _cleanupStats;
        }
        auto sub = BSONObjBuilder(builder.subobjStart("cleanupStats"));
        temp.serialize(&sub);
    }

    if (gTestingDiagnosticsEnabledAtStartup &&
        gUnsupportedDangerousTestingFLEDiagnosticsEnabledAtStartup) {
        auto sub = BSONObjBuilder(builder.subobjStart("emuBinaryStats"));
        sub << "calls" << emuBinaryCalls.loadRelaxed();
        sub << "suboperations" << emuBinarySuboperation.loadRelaxed();
        sub << "totalMillis" << emuBinaryTotalMillis.loadRelaxed();
    }

    return builder.obj();
}


FLEStatusSection::EmuBinaryTracker FLEStatusSection::makeEmuBinaryTracker() {
    return EmuBinaryTracker(this, gTestingDiagnosticsEnabledAtStartup);
}

}  // namespace mongo
