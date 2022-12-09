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

#include <iostream>

#include "mongo/util/processinfo.h"

namespace mongo {

ProcessInfo::ProcessInfo(ProcessId pid) {}

ProcessInfo::~ProcessInfo() {}

bool ProcessInfo::supported() {
    return false;
}

int ProcessInfo::getVirtualMemorySize() {
    return -1;
}

int ProcessInfo::getResidentSize() {
    return -1;
}

double ProcessInfo::getSystemMemoryPressurePercentage() {
    return 0.0;
}

bool ProcessInfo::checkNumaEnabled() {
    return false;
}

bool ProcessInfo::blockCheckSupported() {
    return false;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {}

bool ProcessInfo::blockInMemory(const void* start) {
    verify(0);
}

bool ProcessInfo::pagesInMemory(const void* start, size_t numPages, std::vector<char>* out) {
    verify(0);
}

boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
    return boost::none;
}
}  // namespace mongo
