/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/json.h"

namespace mongo {

    enum ReadPreference {
        /**
         * Read from primary only. All operations produce an error (throw an exception where
         * applicable) if primary is unavailable. Cannot be combined with tags.
         */
        ReadPreference_PrimaryOnly = 0,

        /**
         * Read from primary if available, otherwise a secondary. Tags will only be applied in the
         * event that the primary is unavailable and a secondary is read from. In this event only
         * secondaries matching the tags provided would be read from.
         */
        ReadPreference_PrimaryPreferred,

        /**
         * Read from secondary if available, otherwise error.
         */
        ReadPreference_SecondaryOnly,

        /**
         * Read from a secondary if available, otherwise read from the primary.
         */
        ReadPreference_SecondaryPreferred,

        /**
         * Read from any member.
         */
        ReadPreference_Nearest,
    };


    /**
     * A simple object for representing the list of tags requested by a $readPreference.
     */
    class TagSet {
    public:
        /**
         * Creates a TagSet that matches any nodes.
         *
         * Do not call during static init.
         */
        TagSet();

        /**
         * Creates a TagSet from a BSONArray of tags.
         *
         * @param tags the list of tags associated with this option. This object
         *     will get a shared copy of the list. Therefore, it is important
         *     for the the given tag to live longer than the created tag set.
         */
        explicit TagSet(const BSONArray& tags) : _tags(tags) {}

        /**
         * Returns the BSONArray listing all tags that should be accepted.
         */
        const BSONArray& getTagBSON() const { return _tags; }

        bool operator==(const TagSet& other) const { return _tags == other._tags; }

    private:
        BSONArray _tags;
    };


    struct ReadPreferenceSetting {
        /**
         * @param pref the read preference mode.
         * @param tag the tag set. Note that this object will have the
         *     tag set will have this in a reset state (meaning, this
         *     object's copy of tag will have the iterator in the initial
         *     position).
         */
        ReadPreferenceSetting(ReadPreference pref, const TagSet& tag)
            : pref(pref),
              tags(tag) {

        }

        inline bool equals(const ReadPreferenceSetting& other) const {
            return (pref == other.pref) && (tags == other.tags);
        }

        BSONObj toBSON() const;


        const ReadPreference pref;
        const TagSet tags;
    };

} // namespace mongo
