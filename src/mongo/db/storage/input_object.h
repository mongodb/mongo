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

    virtual const char* getPath() const = 0;

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
