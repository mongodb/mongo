/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include <algorithm>

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    void AccumulatorMedian::processInternal(const Value& input, bool merging) {
        // do nothing with non numeric types
        if (!input.numeric())
            return;

        Super::processInternal(input, merging);
    }

    intrusive_ptr<Accumulator> AccumulatorMedian::create() {
        return new AccumulatorMedian();
    }

    Value AccumulatorMedian::getValue(bool toBeMerged) const {
        if (!toBeMerged) {
            if(vpValue.empty())
                return Value(0.0);

            // can't sort member variable from a const method, so copy out double values and sort
            std::vector<double> vals;
            for(std::vector<Value>::size_type i = 0; i < vpValue.size(); i++) {
                vals.push_back(vpValue[i].coerceToDouble());    
            }

            std::sort(vals.begin(), vals.end());

            if(vals.size() % 2 != 0)
                return Value(vals[vals.size() / 2]);

            return Value((vals[vals.size() / 2] + vals[(vals.size() / 2) - 1]) / 2);
        }
        else {
            return Super::getValue(toBeMerged);
        }
    }

    const char *AccumulatorMedian::getOpName() const {
        return "$median";
    }
}
