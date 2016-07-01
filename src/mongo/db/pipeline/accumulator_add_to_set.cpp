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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

REGISTER_ACCUMULATOR(addToSet, AccumulatorAddToSet::create);

const char* AccumulatorAddToSet::getOpName() const {
    return "$addToSet";
}

void AccumulatorAddToSet::processInternal(const Value& input, bool merging) {
    if (!merging) {
        if (!input.missing()) {
            bool inserted = _set->insert(input).second;
            if (inserted) {
                _memUsageBytes += input.getApproximateSize();
            }
        }
    } else {
        // If we're merging, we need to take apart the arrays we
        // receive and put their elements into the array we are collecting.
        // If we didn't, then we'd get an array of arrays, with one array
        // from each merge source.
        verify(input.getType() == Array);

        const vector<Value>& array = input.getArray();
        for (size_t i = 0; i < array.size(); i++) {
            bool inserted = _set->insert(array[i]).second;
            if (inserted) {
                _memUsageBytes += array[i].getApproximateSize();
            }
        }
    }
}

Value AccumulatorAddToSet::getValue(bool toBeMerged) const {
    return Value(vector<Value>(_set->begin(), _set->end()));
}

AccumulatorAddToSet::AccumulatorAddToSet() {
    _memUsageBytes = sizeof(*this);
}

void AccumulatorAddToSet::reset() {
    _set = getExpressionContext()->getValueComparator().makeUnorderedValueSet();
    _memUsageBytes = sizeof(*this);
}

intrusive_ptr<Accumulator> AccumulatorAddToSet::create() {
    return new AccumulatorAddToSet();
}

void AccumulatorAddToSet::doInjectExpressionContext() {
    _set = getExpressionContext()->getValueComparator().makeUnorderedValueSet();
}

}  // namespace mongo
