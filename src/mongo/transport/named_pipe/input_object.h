// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"

namespace mongo {
// This interface represents any low-level input object such as file descriptor or file handle and
// provides methods to read data synchronously and query its state & metadata. It's modeled after
// C++ standard library ifstream.
//
// TODO Define SeekableInput interface which has the seek() method.
class StreamableInput {
public:
    virtual ~StreamableInput() {}

    virtual const std::string& getAbsolutePath() const = 0;

    void open() {
        if (isOpen()) {
            return;
        }
        doOpen();
    }

    void close() {
        if (isOpen()) {
            doClose();
        }
        tassert(7005013, "State must be 'closed' after closing an input", !isOpen());
    }

    int read(char* data, int count) {
        uassert(7005010, "Input must have been opened before reading", isOpen());
        return doRead(data, count);
    }

    virtual bool isOpen() const = 0;

    virtual bool isGood() const = 0;

    virtual bool isEof() const = 0;

    virtual bool isFailed() const = 0;

protected:
    virtual void doOpen() = 0;

    virtual void doClose() = 0;

    virtual int doRead(char* data, int count) = 0;
};
}  // namespace mongo
