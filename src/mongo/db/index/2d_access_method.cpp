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

#include "mongo/db/index/2d_access_method.h"

#include <string>
#include <vector>

#include "mongo/db/geo/core.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/expression_key_generator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    static double configValueWithDefault(const IndexDescriptor *desc, const string& name, double def) {
        BSONElement e = desc->getInfoElement(name);
        if (e.isNumber()) { return e.numberDouble(); }
        return def;
    }

    TwoDAccessMethod::TwoDAccessMethod(IndexCatalogEntry* btreeState)
        : BtreeBasedAccessMethod(btreeState) {

        const IndexDescriptor* descriptor = btreeState->descriptor();

        BSONObjIterator i(descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            if (e.type() == String && IndexNames::GEO_2D == e.valuestr()) {
                uassert(16800, "can't have 2 geo fields", _params.geo.size() == 0);
                uassert(16801, "2d has to be first in index", _params.other.size() == 0);
                _params.geo = e.fieldName();
            } else {
                int order = 1;
                if (e.isNumber()) {
                    order = static_cast<int>(e.Number());
                }
                _params.other.push_back(make_pair(e.fieldName(), order));
            }
        }
        uassert(16802, "no geo field specified", _params.geo.size());

        double bits =  configValueWithDefault(descriptor, "bits", 26);  // for lat/long, ~ 1ft
        uassert(16803, "bits in geo index must be between 1 and 32", bits > 0 && bits <= 32);

        GeoHashConverter::Parameters params;
        params.bits = static_cast<unsigned>(bits);
        params.max = configValueWithDefault(descriptor, "max", 180.0);
        params.min = configValueWithDefault(descriptor, "min", -180.0);
        double numBuckets = (1024 * 1024 * 1024 * 4.0);
        params.scaling = numBuckets / (params.max - params.min);

        _params.geoHashConverter.reset(new GeoHashConverter(params));
    }

    /** Finds the key objects to put in an index */
    void TwoDAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        get2DKeys(obj, _params, keys, NULL);
    }

    /** Finds all locations in a geo-indexed object */
    void TwoDAccessMethod::getKeys(const BSONObj& obj, vector<BSONObj>& locs) const {
        get2DKeys(obj, _params, NULL, &locs);
    }

}  // namespace mongo
