/**
 * Copyright (C) 2016 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {

class DocumentSourceGeoNear : public DocumentSourceNeedsMongod, public SplittableDocumentSource {
public:
    static const long long kDefaultLimit;

    // virtuals from DocumentSource
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    /**
     * Attempts to combine with a subsequent limit stage, setting the internal limit field
     * as a result.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;
    bool isValidInitialSource() const final {
        return true;
    }
    Value serialize(bool explain = false) const final;
    BSONObjSet getOutputSorts() final {
        return SimpleBSONObjComparator::kInstance.makeBSONObjSet(
            {BSON(distanceField->fullPath() << -1)});
    }

    // Virtuals for SplittableDocumentSource
    boost::intrusive_ptr<DocumentSource> getShardSource() final;
    boost::intrusive_ptr<DocumentSource> getMergeSource() final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    static char geoNearName[];

    long long getLimit() {
        return limit;
    }

    BSONObj getQuery() const {
        return query;
    };

    // this should only be used for testing
    static boost::intrusive_ptr<DocumentSourceGeoNear> create(
        const boost::intrusive_ptr<ExpressionContext>& pCtx);

private:
    explicit DocumentSourceGeoNear(const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void parseOptions(BSONObj options);
    BSONObj buildGeoNearCmd() const;
    void runCommand();

    // These fields describe the command to run.
    // coords and distanceField are required, rest are optional
    BSONObj coords;  // "near" option, but near is a reserved keyword on windows
    bool coordsIsArray;
    std::unique_ptr<FieldPath> distanceField;  // Using unique_ptr because FieldPath can't be empty
    long long limit;
    double maxDistance;
    double minDistance;
    BSONObj query;
    bool spherical;
    double distanceMultiplier;
    std::unique_ptr<FieldPath> includeLocs;

    // these fields are used while processing the results
    BSONObj cmdOutput;
    std::unique_ptr<BSONObjIterator> resultsIterator;  // iterator over cmdOutput["results"]
};

}  // namespace mongo
