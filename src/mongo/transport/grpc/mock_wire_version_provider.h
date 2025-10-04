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

#pragma once

#include "mongo/transport/grpc/util.h"
#include "mongo/transport/grpc/wire_version_provider.h"

namespace mongo::transport::grpc {

/**
 * A WireVersionProvider whose clusterMaxWireVersion can be manually set to arbitrary values.
 */
class MockWireVersionProvider : public WireVersionProvider {
public:
    MockWireVersionProvider() = default;

    int getClusterMaxWireVersion() const override {
        return _clusterMaxWireVersion.load();
    }

    void setClusterMaxWireVersion(int newVersion) {
        _clusterMaxWireVersion.store(newVersion);
    }

private:
    AtomicWord<int> _clusterMaxWireVersion{util::constants::kMinimumWireVersion};
};

}  // namespace mongo::transport::grpc
