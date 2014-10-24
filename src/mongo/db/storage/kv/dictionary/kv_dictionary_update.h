// kv_dictionary_update.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/db/storage/kv/slice.h"

namespace mongo {

    class KVUpdateMessage;

    /**
     * Interface for a serialized update 'message' that may be saved to
     * disk and later replayed against a value to yield a new one.
     *
     * Subclasses implement a few virtual functions that allow the parent
     * class to serialize / deserialize them through the serialize() and
     * fromSerialized() functions respectively.
     *
     * Most importantly, when we fromSerialized() a subclass, we get the
     * correct virtual implementation and so apply() does what we want.
     */
    class KVUpdateMessage {
    public:
        virtual ~KVUpdateMessage() {}

        /**
         * Serialize the state of this message so it can later be
         * deserialized by fromSerialized.
         *
         * Return: owned Slice of bytes representing this update message
         * Requires: fromSerialized(serialized()) yields a KVUpdateMessage
         *           that has the same apply() behavior as this one.
         */
        Slice serialize() const;

        static KVUpdateMessage *fromSerialized(const Slice &serialized);

        /**
         * Apply the logic of this message to `oldValue', yielding
         * `newValue'
         *
         * Requires: The application of this logic against `oldValue' is
         *           known to be error-free.
         * Note: Violatation of the above requirement may result in
         *       undefined behavior, but usually the result is corrupt
         *       data / lost updates.
         * Return: `newValue' set to an owned Slice representing the new value
         *         Status::OK() success
         */
        virtual Status apply(const Slice &oldValue, Slice &newValue) const = 0;

    protected:
        // Unique type tag for the various subclass implementations.
        enum Type {
            UpdateWithDamages,
            UpdateIncrement,
        };

        /**
         * Return: the unique type tag for this particular subclass
         */
        virtual Type getType() const = 0;

        /**
         * Return: the size of this subclasses serialized state
         */
        virtual size_t serializedSize() const = 0;

        /**
         * Serializes the subclass into `dest'.
         *
         * Requires: `dest' has room for at lest `serializedSize()' bytes
         */
        virtual void serializeTo(char *dest) const = 0;
    };

    /**
     * An update message that has all of the information necessary
     * to carry out an 'update with damages'
     *
     * Used by KVRecordStore::updateWithDamages.
     */
    class KVUpdateWithDamagesMessage : public KVUpdateMessage {
    public:
        KVUpdateWithDamagesMessage(const char *source, const mutablebson::DamageVector &damages)
            : _source(source),
              _damages(damages)
        {}

        virtual ~KVUpdateWithDamagesMessage() {}

        static KVUpdateMessage *deserializeFrom(const Slice &serialized);

        virtual Status apply(const Slice &oldValue, Slice &newValue) const;

        // return true once this code is ready
        static bool usable() { return true; }

    protected:
        virtual Type getType() const {
            return UpdateWithDamages;
        }

        virtual size_t serializedSize() const;

        virtual void serializeTo(char *dest) const;

        const char *_source;
        mutablebson::DamageVector _damages;
    };

    /**
     * An update message that increments a little-endian integer.
     *
     * Used by KVRecordStore::_updateStats().
     */
    class KVUpdateIncrementMessage : public KVUpdateMessage {
    public:
        explicit KVUpdateIncrementMessage(int64_t delta)
            : _delta(delta)
        {}

        static KVUpdateMessage *deserializeFrom(const Slice &serialized);

        virtual Status apply(const Slice &oldValue, Slice &newValue) const;

        static bool usable() { return true; }

    protected:
        virtual Type getType() const {
            return UpdateIncrement;
        }

        virtual size_t serializedSize() const;

        virtual void serializeTo(char *dest) const;

        int64_t _delta;
    };

} // namespace mongo
