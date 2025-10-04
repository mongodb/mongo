/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"

#include <memory>

namespace mongo {

/**
 * An integer-keyed container represents a single storage ident that can be written to and read
 * from, where the keys are integers and the values are byte arrays. Multiple container instances
 * may refer to the same ident.
 */
class IntegerKeyedContainer {
public:
    virtual ~IntegerKeyedContainer() {}

    /**
     * Returns the ident of the container.
     */
    virtual std::shared_ptr<Ident> ident() const = 0;

    /**
     * Sets the ident of the container.
     */
    virtual void setIdent(std::shared_ptr<Ident> ident) = 0;

    /**
     * Inserts the given key/value into the container. Must be in an active storage transaction.
     */
    virtual Status insert(RecoveryUnit& ru, int64_t key, std::span<const char> value) = 0;

    /**
     * Removes the given key (and its corresponding value) from the container. Must be in an active
     * storage transaction.
     */
    virtual Status remove(RecoveryUnit& ru, int64_t key) = 0;
};

/**
 * A string-keyed container represents a single storage ident that can be written to and read from,
 * where the keys and values are both byte arrays. Multiple container instances may refer to the
 * same ident.
 */
class StringKeyedContainer {
public:
    virtual ~StringKeyedContainer() {}

    /**
     * Returns the ident of the container.
     */
    virtual std::shared_ptr<Ident> ident() const = 0;

    /**
     * Sets the ident of the container.
     */
    virtual void setIdent(std::shared_ptr<Ident> ident) = 0;

    /**
     * Inserts the given key/value into the container. Must be in an active storage transaction.
     */
    virtual Status insert(RecoveryUnit& ru,
                          std::span<const char> key,
                          std::span<const char> value) = 0;

    /**
     * Removes the given key (and its corresponding value) from the container. Must be in an active
     * storage transaction.
     */
    virtual Status remove(RecoveryUnit& ru, std::span<const char> key) = 0;
};

}  // namespace mongo
