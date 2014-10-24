// kv_dictionary_update.cpp

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

#include "mongo/db/storage/kv/dictionary/kv_dictionary_update.h"
#include "mongo/db/storage/kv/dictionary/simple_serializer.h"
#include "mongo/platform/endian.h"

namespace mongo {

    Slice KVUpdateMessage::serialize() const {
        const size_t size = 1 + serializedSize();
        Slice serialized(size);

        // Set the type byte and then serialize the rest of the message.
        char *data = serialized.mutableData();
        data[0] = getType();
        serializeTo(&data[1]);
        return serialized;
    }

    KVUpdateMessage *KVUpdateMessage::fromSerialized(const Slice &serialized) {
        invariant(serialized.size() > 1);

        // Slice off the type byte.
        const Type type = static_cast<Type>(serialized.data()[0]);
        const Slice slice(serialized.data() + 1, serialized.size() - 1);

        // Dispatch to the right deserialize function
        switch (type) {
        case UpdateWithDamages:
            return KVUpdateWithDamagesMessage::deserializeFrom(slice);
        case UpdateIncrement:
            return KVUpdateIncrementMessage::deserializeFrom(slice);
        default:
            invariant(false);
        }
        return NULL;
    }

    // ---------------------------------------------------------------------- //

    Status KVUpdateWithDamagesMessage::apply(const Slice &oldValue, Slice &newValue) const {
        // Currently, we don't support resizing an element with "updateWithDamages" style updates,
        // so they'll be the same size.
        newValue = oldValue.copy();

        for (mutablebson::DamageVector::const_iterator it = _damages.begin(); it != _damages.end(); it++) {
            const mutablebson::DamageEvent &event = *it;
            invariant(event.targetOffset + event.size < newValue.size());
            std::copy(_source + event.sourceOffset, _source + event.sourceOffset + event.size,
                      newValue.begin() + event.targetOffset);
        }

        return Status::OK();
    }

    size_t KVUpdateWithDamagesMessage::serializedSize() const {
        size_t totalSize = sizeof(size_t); // number of events
        for (mutablebson::DamageVector::const_iterator it = _damages.begin(); it != _damages.end(); it++) {
            const mutablebson::DamageEvent &event = *it;
            // offset and size of each event, plus the actual data for the event
            totalSize += sizeof(mutablebson::DamageEvent::OffsetSizeType) +
                         sizeof(size_t) +
                         event.size;
        }
        return totalSize;
    }

    // The serialized format is a header of _damages.size() offset/size pairs, followed by a buffer
    // containing all damage events packed together.  The header entries only store the target
    // offset and size, we know the serialized events are packed together so we don't need to
    // serialize source offsets, we can recalculate them on deserialize.
    void KVUpdateWithDamagesMessage::serializeTo(char *dest) const {
        BufferWriter writer(dest);

        // Write the number of entries.
        writer.write(_damages.size());

        // Copy header entries.
        for (mutablebson::DamageVector::const_iterator it = _damages.begin(); it != _damages.end(); it++) {
            const mutablebson::DamageEvent &event = *it;
            writer.write(event.targetOffset);
            writer.write(event.size);
        }

        // Copy damage events, packing them together at the end.
        char *eventStart = writer.get();
        for (mutablebson::DamageVector::const_iterator it = _damages.begin(); it != _damages.end(); it++) {
            const mutablebson::DamageEvent &event = *it;
            std::copy(_source + event.sourceOffset, _source + event.sourceOffset + event.size,
                      eventStart);
            eventStart += event.size;
        }
    }

    KVUpdateMessage *KVUpdateWithDamagesMessage::deserializeFrom(const Slice &serialized) {
        BufferReader reader(serialized.data());

        // Read the number of entries.
        invariant(serialized.size() >= sizeof(size_t));
        const size_t numEntries = reader.read<size_t>();

        // Check that the slice is big enough for all the header entries.
        invariant(serialized.size() >=
                  sizeof(size_t) + (numEntries *
                                    (sizeof(mutablebson::DamageEvent::OffsetSizeType) + sizeof(size_t))));

        mutablebson::DamageVector vec;
        vec.reserve(numEntries);

        // Read all the headers.  Track the current "position" of the sourceOffset as we go along.
        mutablebson::DamageEvent::OffsetSizeType currentSourceOffset = 0;
        size_t totalDamagesSize = 0;
        for (size_t i = 0; i < numEntries; ++i) {
            mutablebson::DamageEvent event;
            event.sourceOffset = currentSourceOffset;
            event.targetOffset = reader.read<mutablebson::DamageEvent::OffsetSizeType>();
            event.size = reader.read<size_t>();
            currentSourceOffset += event.size;
            totalDamagesSize += event.size;

            vec.push_back(event);
        }

        const char *source = reader.get();

        // Check that the remaining data is of the right size for all the damage entry data.
        invariant(serialized.size() == (source - serialized.data()) + totalDamagesSize);

        return new KVUpdateWithDamagesMessage(source, vec);
    }

    // ---------------------------------------------------------------------- //

    Status KVUpdateIncrementMessage::apply(const Slice &oldValue, Slice &newValue) const {
        const int64_t newVal = mongo::endian::nativeToLittle(
            mongo::endian::littleToNative(oldValue.as<int64_t>()) + _delta);
        newValue = Slice::of(newVal).owned();
        return Status::OK();
    }

    size_t KVUpdateIncrementMessage::serializedSize() const {
        return sizeof(int64_t);
    }

    void KVUpdateIncrementMessage::serializeTo(char *dest) const {
        BufferWriter writer(dest);

        const int64_t littleDelta = mongo::endian::nativeToLittle(_delta);
        writer.write(littleDelta);
    }

    KVUpdateMessage *KVUpdateIncrementMessage::deserializeFrom(const Slice &serialized) {
        BufferReader reader(serialized.data());

        invariant(serialized.size() == sizeof(int64_t));
        const int64_t delta = mongo::endian::littleToNative(reader.read<int64_t>());

        return new KVUpdateIncrementMessage(delta);
    }

} // namespace mongo

