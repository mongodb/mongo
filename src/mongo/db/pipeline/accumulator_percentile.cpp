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

#include "third_party/folly/TDigest.h"

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

const char sumName[] = "sum";
const char countName[] = "count";
const char maxName[] = "max";
const char minName[] = "min";
const char percentileName[] = "percentile";
const char digestSizeName[] = "digest_size";
const char centroidsName[] = "centroids";
const char meanName[] = "mean";
const char weightName[] = "weight";
}  // namespace

void AccumulatorPercentile::processInternal(const Value& input, bool merging) {

    if (merging) {
        verify(input.getType() == Object);

        Value digest_centroids = input[centroidsName];
        double digest_sum = input[sumName].getDouble();
        double digest_count = input[countName].getDouble();
        double digest_max = input[maxName].getDouble();
        double digest_min = input[minName].getDouble();
        double digest_size = input[digestSizeName].getDouble();

        std::vector<mongo::TDigest::Centroid> centroids;
        for (const auto& centroid: digest_centroids.getArray()) {
            centroids.push_back(mongo::TDigest::Centroid(centroid[meanName].getDouble(), centroid[weightName].getDouble()));
        };

        digest = digest.merge({mongo::TDigest(centroids, digest_sum, digest_count, digest_max, digest_min, digest_size), digest});
        this->percentile = input[percentileName].getDouble();
        return;
    }

    // Determining 'digest_size'
    if (this->digest_size == 0){
        if (input.getDocument()["digest_size"].missing())
            {
            this->digest_size = 1000;
            }
        else
            {
            this->digest_size = input.getDocument()["digest_size"].getDouble();
            }
    }

    uassert(51300, "The 'percentile' should be present in the input document.",
    !input.getDocument()["percentile"].missing());

    uassert(51301, "The 'value' should be present in the input document.",
    !input.getDocument()["value"].missing());

    this->percentile = input.getDocument()["percentile"].getDouble();

    Value input_value = input.getDocument()["value"];

    switch (input_value.getType()) {
        case NumberDecimal:
        case NumberLong:
        case NumberInt:
        case NumberDouble:
            values.push_back(input_value.getDouble());
            break;
        default:
            dassert(!input_value.numeric());
            return;
    }
    
    if (any_input == false)
    {
        digest = mongo::TDigest(this->digest_size);
        any_input = true;
    }

    if (values.size() == this->chunk_size){
        _add_to_tdigest(values);
        }
}

intrusive_ptr<Accumulator> AccumulatorPercentile::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new AccumulatorPercentile(expCtx);
}

Value AccumulatorPercentile::getValue(bool toBeMerged) {

    // To add the remainders
    if (not values.empty()){
        _add_to_tdigest(values);
        }

    if (toBeMerged) {
        std::vector<Document> centroids;

        for (const auto& centroid: this->digest.getCentroids()) {
            centroids.push_back(Document{
                    {"mean", centroid.mean()},
                    {"weight", centroid.weight()}
                });
        };
        
        Value res = Value(
            Document{
                {"centroids", Value(centroids)},
                {"sum", digest.sum()},
                {"count", digest.count()},
                {"max", digest.max()},
                {"min", digest.min()},
                {"percentile", this->percentile},
                {"digest_size", this->digest_size}
            }
        );
        return res;
    }
    return Value(digest.estimateQuantile(this->percentile));
}

AccumulatorPercentile::AccumulatorPercentile(const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Accumulator(expCtx) {
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
    this->digest_size = 0;
    values.clear();
    digest = mongo::TDigest(this->digest_size);
    any_input = false;
}
}
