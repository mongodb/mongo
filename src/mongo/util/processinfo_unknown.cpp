// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/processinfo.h"

#include <iostream>

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

void ProcessInfo::SystemInfo::collectSystemInfo() {}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {}

boost::optional<uint64_t> ProcessInfo::getNumCoresForProcess() {
    return boost::none;
}
}  // namespace mongo
