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

#pragma once

#include <memory>

namespace mongo {

class OperationContext;

/**
 * This class allows for the storageEngine to alert the rest of the system about journaled write
 * progress.
 *
 * It has two methods. The first, getToken(), returns a token representing the current progress
 * applied to the node. It should be called just prior to making writes durable (usually, syncing a
 * journal entry to disk).
 *
 * The second method, onDurable(), takes this token as an argument and relays to the rest of the
 * system that writes through that point have been journaled.
 */
class JournalListener {
public:
    class Token {
    public:
        virtual ~Token() = default;
    };
    virtual ~JournalListener() = default;
    virtual std::unique_ptr<Token> getToken(OperationContext* opCtx) = 0;
    virtual void onDurable(const Token& token) = 0;
};

}  // namespace mongo
