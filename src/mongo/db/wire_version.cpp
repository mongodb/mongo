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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/db/wire_version.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/thread_safety_context.h"

namespace mongo {

WireSpec& WireSpec::instance() {
    static StaticImmortal<WireSpec> instance;
    return *instance;
}

void WireSpec::appendInternalClientWireVersion(WireVersionInfo wireVersionInfo,
                                               BSONObjBuilder* builder) {
    BSONObjBuilder subBuilder(builder->subobjStart("internalClient"));
    WireVersionInfo::appendToBSON(wireVersionInfo, &subBuilder);
}

BSONObj specToBSON(const WireSpec::Specification& spec) {
    BSONObjBuilder bob;
    WireSpec::Specification::appendToBSON(spec, &bob);
    return bob.obj();
}

void WireSpec::initialize(Specification spec) {
    invariant(ThreadSafetyContext::getThreadSafetyContext()->isSingleThreaded());
    fassert(ErrorCodes::AlreadyInitialized, !isInitialized());
    BSONObj newSpec = specToBSON(spec);
    _spec = std::make_shared<Specification>(std::move(spec));
    LOGV2(4915701, "Initialized wire specification", "spec"_attr = newSpec);
}

void WireSpec::reset(Specification spec) {
    BSONObj oldSpec, newSpec;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        internalAssert(
            ErrorCodes::NotYetInitialized, "WireSpec is not yet initialized", isInitialized());

        oldSpec = specToBSON(*_spec.get());
        _spec = std::make_shared<Specification>(std::move(spec));
        newSpec = specToBSON(*_spec.get());
    }

    LOGV2(
        4915702, "Updated wire specification", "oldSpec"_attr = oldSpec, "newSpec"_attr = newSpec);
}

std::shared_ptr<const WireSpec::Specification> WireSpec::get() const {
    stdx::lock_guard<Latch> lk(_mutex);
    fassert(ErrorCodes::NotYetInitialized, isInitialized());
    return _spec;
}

}  // namespace mongo
