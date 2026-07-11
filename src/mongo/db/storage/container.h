// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <memory>
#include <span>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace container {

/**
 * The policy for inserting into a container when the provided key already exists.
 */
enum class ExistingKeyPolicy {
    // Overwrite the existing value at the given key with the newly-provided one.
    overwrite,
    // Reject the write, returning an error.
    reject,
};

/**
 * The format for container update oplog entries.
 */
enum class UpdateOplogEntryVersion {
    // Full replacement semantics, currently the only supported oplog format.
    kFullReplacementV1 = 1,

    // Must be last.
    kNumVersions
};

}  // namespace container

/**
 * An integer-keyed container represents a single storage ident that can be written to and read
 * from, where the keys are integers and the values are byte arrays. Multiple container instances
 * may refer to the same ident.
 */
class [[MONGO_MOD_OPEN]] IntegerKeyedContainer {
public:
    class [[MONGO_MOD_OPEN]] Cursor {
    public:
        virtual ~Cursor() = default;

        /**
         * Returns the value in the container at the given key, or none if it is not present.
         */
        virtual boost::optional<std::span<const char>> find(int64_t key) = 0;

        /**
         * Returns the next key/value in the container, or none if the cursor has reached the end.
         */
        virtual boost::optional<std::pair<int64_t, std::span<const char>>> next() = 0;
    };

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
    virtual Status insert(RecoveryUnit& ru,
                          int64_t key,
                          std::span<const char> value,
                          container::ExistingKeyPolicy policy) = 0;

    /**
     * Updates the value at the given key. The key must already exist. Must be in an active storage
     * transaction.
     */
    virtual Status update(RecoveryUnit& ru, int64_t key, std::span<const char> value) = 0;

    /**
     * Removes the given key (and its corresponding value) from the container. Must be in an active
     * storage transaction.
     */
    virtual Status remove(RecoveryUnit& ru, int64_t key) = 0;

    /**
     * Returns a cursor on this container.
     */
    virtual std::unique_ptr<Cursor> getCursor(RecoveryUnit& ru) const = 0;
};

/**
 * A string-keyed container represents a single storage ident that can be written to and read from,
 * where the keys and values are both byte arrays. Multiple container instances may refer to the
 * same ident.
 */
class [[MONGO_MOD_OPEN]] StringKeyedContainer {
public:
    class [[MONGO_MOD_OPEN]] Cursor {
    public:
        virtual ~Cursor() = default;

        /**
         * Returns the value in the container at the given key, or none if it is not present.
         */
        virtual boost::optional<std::span<const char>> find(std::span<const char> key) = 0;

        /**
         * Returns the next key/value in the container, or none if the cursor has reached the end.
         */
        virtual boost::optional<std::pair<std::span<const char>, std::span<const char>>> next() = 0;
    };

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
                          std::span<const char> value,
                          container::ExistingKeyPolicy policy) = 0;

    /**
     * Updates the value at the given key. The key must already exist. Must be in an active storage
     * transaction.
     */
    virtual Status update(RecoveryUnit& ru,
                          std::span<const char> key,
                          std::span<const char> value) = 0;

    /**
     * Removes the given key (and its corresponding value) from the container. Must be in an active
     * storage transaction.
     */
    virtual Status remove(RecoveryUnit& ru, std::span<const char> key) = 0;

    /**
     * Returns a cursor on this container.
     */
    virtual std::unique_ptr<Cursor> getCursor(RecoveryUnit& ru) const = 0;
};

}  // namespace mongo
