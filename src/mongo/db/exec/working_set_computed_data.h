/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/working_set.h"

namespace mongo {

    class TextScoreComputedData : public WorkingSetComputedData {
    public:
        TextScoreComputedData(double score)
            : WorkingSetComputedData(WSM_COMPUTED_TEXT_SCORE),
              _score(score) { }

        double getScore() const { return _score; }

        virtual TextScoreComputedData* clone() const {
            return new TextScoreComputedData(_score);
        }

    private:
        double _score;
    };

    class GeoDistanceComputedData : public WorkingSetComputedData {
    public:
        GeoDistanceComputedData(double score)
            : WorkingSetComputedData(WSM_COMPUTED_GEO_DISTANCE),
              _score(score) { }

        double getScore() const { return _score; }

        virtual GeoDistanceComputedData* clone() const {
            return new GeoDistanceComputedData(_score);
        }

    private:
        double _score;
    };

    class IndexKeyComputedData : public WorkingSetComputedData {
    public:
        IndexKeyComputedData(BSONObj key)
            : WorkingSetComputedData(WSM_INDEX_KEY),
              _key(key) { }

        BSONObj getKey() const { return _key; }

        virtual IndexKeyComputedData* clone() const {
            return new IndexKeyComputedData(_key);
        }

    private:
        BSONObj _key;
    };

}  // namespace mongo
