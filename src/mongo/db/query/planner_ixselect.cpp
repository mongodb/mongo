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

#include "mongo/db/query/planner_ixselect.h"

#include <vector>

#include "mongo/db/geo/core.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo {

    static double fieldWithDefault(const BSONObj& infoObj, const string& name, double def) {
        BSONElement e = infoObj[name];
        if (e.isNumber()) { return e.numberDouble(); }
        return def;
    }

    /**
     * 2d indices don't handle wrapping so we can't use them for queries that wrap.
     */
    static bool twoDWontWrap(const Circle& circle, const IndexEntry& index) {
        // XXX: where does this really belong
        GeoHashConverter::Parameters params;
        params.bits = static_cast<unsigned>(fieldWithDefault(index.infoObj, "bits", 26));
        params.max = fieldWithDefault(index.infoObj, "max", 180.0);
        params.min = fieldWithDefault(index.infoObj, "min", -180.0);
        double numBuckets = (1024 * 1024 * 1024 * 4.0);
        params.scaling = numBuckets / (params.max - params.min);

        GeoHashConverter conv(params);

        // FYI: old code used flat not spherical error.
        double yscandist = rad2deg(circle.radius) + conv.getErrorSphere();
        double xscandist = computeXScanDistance(circle.center.y, yscandist);
        bool ret = circle.center.x + xscandist < 180
                && circle.center.x - xscandist > -180
                && circle.center.y + yscandist < 90
                && circle.center.y - yscandist > -90;
        return ret;
    }

    // static
    void QueryPlannerIXSelect::getFields(MatchExpression* node,
                                         string prefix,
                                         unordered_set<string>* out) {
        // Do not traverse tree beyond negation node
        MatchExpression::MatchType exprtype = node->matchType();
        if (exprtype == MatchExpression::NOT || exprtype == MatchExpression::NOR) {
            return;
        }

        // Leaf nodes with a path and some array operators.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            out->insert(prefix + node->path().toString());
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // If the array uses an index on its children, it's something like
            // {foo : {$elemMatch: { bar: 1}}}, in which case the predicate is really over
            // foo.bar.
            //
            // When we have {foo: {$all: [{$elemMatch: {a:1}}], the path of the embedded elemMatch
            // is empty.  We don't want to append a dot in that case as the field would be foo..a.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }

            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                getFields(node->getChild(i), prefix, out);
            }
        }
    }

    // static
    void QueryPlannerIXSelect::findRelevantIndices(const unordered_set<string>& fields,
                                                   const vector<IndexEntry>& allIndices,
                                                   vector<IndexEntry>* out) {
        for (size_t i = 0; i < allIndices.size(); ++i) {
            BSONObjIterator it(allIndices[i].keyPattern);
            verify(it.more());
            BSONElement elt = it.next();
            if (fields.end() != fields.find(elt.fieldName())) {
                out->push_back(allIndices[i]);
            }
        }
    }

    // static
    bool QueryPlannerIXSelect::compatible(const BSONElement& elt,
                                          const IndexEntry& index,
                                          MatchExpression* node) {
        // XXX: CatalogHack::getAccessMethodName: do we have to worry about this?  when?
        string ixtype;
        if (String != elt.type()) {
            ixtype = "";
        }
        else {
            ixtype = elt.String();
        }

        // We know elt.fieldname() == node->path().
        MatchExpression::MatchType exprtype = node->matchType();

        // TODO: use indexnames
        if ("" == ixtype) {
            if (index.sparse && exprtype == MatchExpression::EQ) {
                // Can't check for null w/a sparse index.
                const EqualityMatchExpression* expr
                    = static_cast<const EqualityMatchExpression*>(node);
                return !expr->getData().isNull();
            }
            return exprtype != MatchExpression::GEO && exprtype != MatchExpression::GEO_NEAR;
        }
        else if ("hashed" == ixtype) {
            return exprtype == MatchExpression::MATCH_IN || exprtype == MatchExpression::EQ;
        }
        else if ("2dsphere" == ixtype) {
            if (exprtype == MatchExpression::GEO) {
                // within or intersect.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                const GeometryContainer& gc = gq.getGeometry();
                return gc.hasS2Region();
            }
            else if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                // Make sure the near query is compatible with 2dsphere.
                if (gnme->getData().centroid.crs == SPHERE || gnme->getData().isNearSphere) {
                    return true;
                }
            }
            return false;
        }
        else if ("2d" == ixtype) {
            if (exprtype == MatchExpression::GEO_NEAR) {
                GeoNearMatchExpression* gnme = static_cast<GeoNearMatchExpression*>(node);
                return gnme->getData().centroid.crs == FLAT;
            }
            else if (exprtype == MatchExpression::GEO) {
                // 2d only supports within.
                GeoMatchExpression* gme = static_cast<GeoMatchExpression*>(node);
                const GeoQuery& gq = gme->getGeoQuery();
                if (GeoQuery::WITHIN != gq.getPred()) {
                    return false;
                }

                const GeometryContainer& gc = gq.getGeometry();

                // 2d indices answer flat queries.
                if (gc.hasFlatRegion()) {
                    return true;
                }

                // 2d indices can answer centerSphere queries.
                if (NULL == gc._cap.get()) {
                    return false;
                }

                verify(SPHERE == gc._cap->crs);
                const Circle& circle = gc._cap->circle;

                // No wrapping around the edge of the world is allowed in 2d centerSphere.
                return twoDWontWrap(circle, index);
            }
            return false;
        }
        else if ("text" == ixtype) {
            return (exprtype == MatchExpression::TEXT);
        }
        else if ("geoHaystack" == ixtype) {
            return false;
        }
        else {
            warning() << "Unknown indexing for node " << node->toString()
                      << " and field " << elt.toString() << endl;
            verify(0);
        }
    }

    // static
    void QueryPlannerIXSelect::rateIndices(MatchExpression* node,
                                           string prefix,
                                           const vector<IndexEntry>& indices) {
        // Do not traverse tree beyond negation node
        MatchExpression::MatchType exprtype = node->matchType();
        if (exprtype == MatchExpression::NOT || exprtype == MatchExpression::NOR) {
            return;
        }

        // Every indexable node is tagged even when no compatible index is
        // available.
        if (Indexability::nodeCanUseIndexOnOwnField(node)) {
            string fullPath = prefix + node->path().toString();
            verify(NULL == node->getTag());
            RelevantTag* rt = new RelevantTag();
            node->setTag(rt);
            rt->path = fullPath;

            // TODO: This is slow, with all the string compares.
            for (size_t i = 0; i < indices.size(); ++i) {
                BSONObjIterator it(indices[i].keyPattern);
                BSONElement elt = it.next();
                if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                    rt->first.push_back(i);
                }
                while (it.more()) {
                    elt = it.next();
                    if (elt.fieldName() == fullPath && compatible(elt, indices[i], node)) {
                        rt->notFirst.push_back(i);
                    }
                }
            }
        }
        else if (Indexability::arrayUsesIndexOnChildren(node)) {
            // See comment in getFields about all/elemMatch and paths.
            if (!node->path().empty()) {
                prefix += node->path().toString() + ".";
            }
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
        else if (node->isLogical()) {
            for (size_t i = 0; i < node->numChildren(); ++i) {
                rateIndices(node->getChild(i), prefix, indices);
            }
        }
    }

}  // namespace mongo
