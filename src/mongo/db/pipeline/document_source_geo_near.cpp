/**
 * Copyright 2011 (c) 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_geo_near.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(geoNear,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceGeoNear::createFromBson);

const long long DocumentSourceGeoNear::kDefaultLimit = 100;

const char* DocumentSourceGeoNear::getSourceName() const {
    return "$geoNear";
}

DocumentSource::GetNextResult DocumentSourceGeoNear::getNext() {
    pExpCtx->checkForInterrupt();

    if (!resultsIterator)
        runCommand();

    if (!resultsIterator->more())
        return GetNextResult::makeEOF();

    // Each result from the geoNear command is wrapped in a wrapper object with "obj",
    // "dis" and maybe "loc" fields. We want to take the object from "obj" and inject the
    // other fields into it.
    Document result(resultsIterator->next().embeddedObject());
    MutableDocument output(result["obj"].getDocument());
    output.setNestedField(*distanceField, result["dis"]);
    if (includeLocs)
        output.setNestedField(*includeLocs, result["loc"]);

    // In a cluster, $geoNear output will be merged via $sort, so add the sort key.
    if (pExpCtx->needsMerge) {
        output.setSortKeyMetaField(BSON("" << result["dis"]));
    }

    return output.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceGeoNear::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        // If the next stage is a $limit, we can combine it with ourselves.
        limit = std::min(limit, nextLimit->getLimit());
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

// This command is sent as-is to the shards.
intrusive_ptr<DocumentSource> DocumentSourceGeoNear::getShardSource() {
    return this;
}
// On mongoS this becomes a merge sort by distance (nearest-first) with limit.
std::list<intrusive_ptr<DocumentSource>> DocumentSourceGeoNear::getMergeSources() {
    return {DocumentSourceSort::create(
        pExpCtx, BSON(distanceField->fullPath() << 1 << "$mergePresorted" << true), limit)};
}

Value DocumentSourceGeoNear::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument result;

    if (coordsIsArray) {
        result.setField("near", Value(BSONArray(coords)));
    } else {
        result.setField("near", Value(coords));
    }

    // not in buildGeoNearCmd
    result.setField("distanceField", Value(distanceField->fullPath()));

    result.setField("limit", Value(limit));

    if (maxDistance > 0)
        result.setField("maxDistance", Value(maxDistance));

    if (minDistance > 0)
        result.setField("minDistance", Value(minDistance));

    result.setField("query", Value(query));
    result.setField("spherical", Value(spherical));
    result.setField("distanceMultiplier", Value(distanceMultiplier));

    if (includeLocs)
        result.setField("includeLocs", Value(includeLocs->fullPath()));

    return Value(DOC(getSourceName() << result.freeze()));
}

BSONObj DocumentSourceGeoNear::buildGeoNearCmd() const {
    // this is very similar to sourceToBson, but slightly different.
    // differences will be noted.

    BSONObjBuilder geoNear;  // not building a subField

    geoNear.append("geoNear", pExpCtx->ns.coll());  // not in toBson

    if (coordsIsArray) {
        geoNear.appendArray("near", coords);
    } else {
        geoNear.append("near", coords);
    }

    geoNear.append("num", limit);  // called limit in toBson

    if (maxDistance > 0)
        geoNear.append("maxDistance", maxDistance);

    if (minDistance > 0)
        geoNear.append("minDistance", minDistance);

    geoNear.append("query", query);
    if (pExpCtx->getCollator()) {
        geoNear.append("collation", pExpCtx->getCollator()->getSpec().toBSON());
    } else {
        geoNear.append("collation", CollationSpec::kSimpleSpec);
    }

    geoNear.append("spherical", spherical);
    geoNear.append("distanceMultiplier", distanceMultiplier);

    if (includeLocs)
        geoNear.append("includeLocs", true);  // String in toBson

    return geoNear.obj();
}

void DocumentSourceGeoNear::runCommand() {
    massert(16603, "Already ran geoNearCommand", !resultsIterator);

    bool ok = _mongoProcessInterface->directClient()->runCommand(
        pExpCtx->ns.db().toString(), buildGeoNearCmd(), cmdOutput);
    uassert(16604, "geoNear command failed: " + cmdOutput.toString(), ok);

    resultsIterator.reset(new BSONObjIterator(cmdOutput["results"].embeddedObject()));
}

intrusive_ptr<DocumentSourceGeoNear> DocumentSourceGeoNear::create(
    const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> source(new DocumentSourceGeoNear(pCtx));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGeoNear::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pCtx) {
    intrusive_ptr<DocumentSourceGeoNear> out = new DocumentSourceGeoNear(pCtx);
    out->parseOptions(elem.embeddedObjectUserCheck());
    return out;
}

void DocumentSourceGeoNear::parseOptions(BSONObj options) {
    // near and distanceField are required

    uassert(16605,
            "$geoNear requires a 'near' option as an Array",
            options["near"].isABSONObj());  // Array or Object (Object is deprecated)
    coordsIsArray = options["near"].type() == Array;
    coords = options["near"].embeddedObject().getOwned();

    uassert(16606,
            "$geoNear requires a 'distanceField' option as a String",
            options["distanceField"].type() == String);
    distanceField.reset(new FieldPath(options["distanceField"].str()));

    // remaining fields are optional

    // num and limit are synonyms
    if (options["limit"].isNumber())
        limit = options["limit"].numberLong();
    if (options["num"].isNumber())
        limit = options["num"].numberLong();

    if (options["maxDistance"].isNumber())
        maxDistance = options["maxDistance"].numberDouble();

    if (options["minDistance"].isNumber())
        minDistance = options["minDistance"].numberDouble();

    if (options["query"].type() == Object)
        query = options["query"].embeddedObject().getOwned();

    spherical = options["spherical"].trueValue();

    if (options["distanceMultiplier"].isNumber())
        distanceMultiplier = options["distanceMultiplier"].numberDouble();

    if (options.hasField("includeLocs")) {
        uassert(16607,
                "$geoNear requires that 'includeLocs' option is a String",
                options["includeLocs"].type() == String);
        includeLocs.reset(new FieldPath(options["includeLocs"].str()));
    }

    if (options.hasField("uniqueDocs"))
        warning() << "ignoring deprecated uniqueDocs option in $geoNear aggregation stage";

    // The collation field is disallowed, even though it is accepted by the geoNear command, since
    // the $geoNear operation should respect the collation associated with the entire pipeline.
    uassert(40227,
            "$geoNear does not accept the 'collation' parameter. Instead, specify a collation "
            "for the entire aggregation command.",
            !options["collation"]);
}

DocumentSourceGeoNear::DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSourceNeedsMongoProcessInterface(pExpCtx),
      coordsIsArray(false),
      limit(DocumentSourceGeoNear::kDefaultLimit),
      maxDistance(-1.0),
      minDistance(-1.0),
      spherical(false),
      distanceMultiplier(1.0) {}
}
