// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/environment_buffer.h"

#ifndef _WIN32
extern char** environ;
namespace {
char** environmentPointer = environ;
}
#else
namespace {
char** environmentPointer = nullptr;
}

#endif

char** mongo::getEnvironPointer() {
    return environmentPointer;
}
