/*
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

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/platform/decimal128.h"

#include "mongo/db/pipeline/TDigest.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(percentile, AccumulatorPercentile::create);
REGISTER_EXPRESSION(percentile, ExpressionFromAccumulator<AccumulatorPercentile>::parse);


const char* AccumulatorPercentile::getOpName() const {
    return "$percentile";
}

namespace {
const char subTotalName[] = "subTotal";
const char subTotalErrorName[] = "subTotalError";  // Used for extra precision
const char countName[] = "count";
}  // namespace

void AccumulatorPercentile::processInternal(const Value& input, bool merging) {

    // Determining 'digest_size'
    if (this->digest_size == 0){
        
        if (input.getDocument()["digest_size"].missing()){
            this->digest_size = 1000;
            }
        else
            {
            this->digest_size = input.getDocument()["digest_size"].getDouble();
            }
    }

    // ToDo: error codes are not accurate. Set better numbers later
    // ToDo: It might be better evaluations for this part.
    uassert(6677, "The 'perc' should be present in the input document.",
    !input.getDocument()["perc"].missing());

    uassert(6678, "The 'value' should be present in the input document.",
    !input.getDocument()["value"].missing());

    this->perc_val = input.getDocument()["perc"].getDouble() / 100;  // Converting Percentile to Quantile - [0:100] to [0:1]

    // ToDo: Choose a better name for perc_input and refactor later
    Value perc_input = input.getDocument()["value"];


    if (merging) {
        // We expect an object that contains both a subtotal and a count. Additionally there may
        // be an error value, that allows for additional precision.
        // 'input' is what getValue(true) produced below.
        verify(perc_input.getType() == Object);
        // We're recursively adding the subtotal to get the proper type treatment, but this only
        // increments the count by one, so adjust the count afterwards. Similarly for 'error'.
        processInternal(perc_input[subTotalName], false);
        _count += perc_input[countName].getLong() - 1;
        Value error = perc_input[subTotalErrorName];
        if (!error.missing()) {
            processInternal(error, false);
            _count--;  // The error correction only adjusts the total, not the number of items.
        }
        return;
    }

    // ToDo: Not sure 1) Is it important for TDigest to distinguish?  2) Is it important for MongoDB to distinguish?
    // ToDo: Going to cover all Decimal, Long and Double as a temporary, need to decide on this.
    switch (perc_input.getType()) {

        case NumberDecimal:
        case NumberLong:
        case NumberInt:
        case NumberDouble:
            values.push_back(perc_input.getDouble());
            break;
        default:
            dassert(!perc_input.numeric());
            return;
    }
    _count++;

    if (values.size() == this->chunk_size){
        _add_to_tdigest(values);
        }
    else
        return;
}

intrusive_ptr<Accumulator> AccumulatorPercentile::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new AccumulatorPercentile(expCtx);
}

Decimal128 AccumulatorPercentile::_getDecimalTotal() const {
    return _decimalTotal.add(_nonDecimalTotal.getDecimal());
}

Value AccumulatorPercentile::getValue(bool toBeMerged) {

    // To add remainders left over a chunk
    if (values.size() > 0){
        _add_to_tdigest(values);
        }

    // ToDo: Unchanged copy from 'avg' module, need to change this for Percentile
    if (toBeMerged) {
        if (_isDecimal)
            return Value(Document{{subTotalName, _getDecimalTotal()}, {countName, _count}});

        double total, error;
        std::tie(total, error) = _nonDecimalTotal.getDoubleDouble();
        return Value(
            Document{{subTotalName, total}, {countName, _count}, {subTotalErrorName, error}});
    }

    if (_count == 0){
        return Value(BSONNULL);
        }

    return Value(this->digest.estimateQuantile(this->perc_val));
}

AccumulatorPercentile::AccumulatorPercentile(const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Accumulator(expCtx), _isDecimal(false), _count(0) {

    // Higher 'digest_size' results in higher memory consumption and better precision
    mongo::TDigest digest(this->digest_size);

    // This is a fixed size Accumulator so we never need to update this
    _memUsageBytes = sizeof(*this);
}

void AccumulatorPercentile::_add_to_tdigest(std::vector<double> & values){

    // Sort, Push and Clear the "values" vector in each chunk
    std::sort(values.begin(), values.end());
    digest = digest.merge(values);
    values.clear();
}

void AccumulatorPercentile::reset() {
    _isDecimal = false;
    _nonDecimalTotal = {};
    _decimalTotal = {};
    _count = 0;
}
}
