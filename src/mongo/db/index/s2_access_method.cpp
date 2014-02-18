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

#include "mongo/db/index/s2_access_method.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/expression_key_generator.h"
#include "mongo/db/jsobj.h"

namespace mongo {
    static const string kIndexVersionFieldName("2dsphereIndexVersion");

    static int configValueWithDefault(const IndexDescriptor *desc, const string& name, int def) {
        BSONElement e = desc->getInfoElement(name);
        if (e.isNumber()) { return e.numberInt(); }
        return def;
    }

    S2AccessMethod::S2AccessMethod(IndexCatalogEntry* btreeState)
        : BtreeBasedAccessMethod(btreeState) {

        const IndexDescriptor* descriptor = btreeState->descriptor();

        // Set up basic params.
        _params.maxKeysPerInsert = 200;
        // This is advisory.
        _params.maxCellsInCovering = 50;
        // Near distances are specified in meters...sometimes.
        _params.radius = kRadiusOfEarthInMeters;
        // These are not advisory.
        _params.finestIndexedLevel = configValueWithDefault(descriptor, "finestIndexedLevel",
            S2::kAvgEdge.GetClosestLevel(500.0 / _params.radius));
        _params.coarsestIndexedLevel = configValueWithDefault(descriptor, "coarsestIndexedLevel",
            S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / _params.radius));
        // Determine which version of this index we're using.  If none was set in the descriptor,
        // assume S2_INDEX_VERSION_1 (alas, the first version predates the existence of the version
        // field).
        _params.indexVersion = static_cast<S2IndexVersion>(configValueWithDefault(
            descriptor, kIndexVersionFieldName, S2_INDEX_VERSION_1));
        uassert(16747, "coarsestIndexedLevel must be >= 0", _params.coarsestIndexedLevel >= 0);
        uassert(16748, "finestIndexedLevel must be <= 30", _params.finestIndexedLevel <= 30);
        uassert(16749, "finestIndexedLevel must be >= coarsestIndexedLevel",
                _params.finestIndexedLevel >= _params.coarsestIndexedLevel);
        massert(17395,
                str::stream() << "unsupported geo index version { " << kIndexVersionFieldName
                              << " : " << _params.indexVersion << " }, only support versions: ["
                              << S2_INDEX_VERSION_1 << "," << S2_INDEX_VERSION_2 << "]",
                _params.indexVersion == S2_INDEX_VERSION_2
                    || _params.indexVersion == S2_INDEX_VERSION_1);

        int geoFields = 0;

        // Categorize the fields we're indexing and make sure we have a geo field.
        BSONObjIterator i(descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            if (e.type() == String && IndexNames::GEO_2DSPHERE == e.String() ) {
                ++geoFields;
            }
            else {
                // We check for numeric in 2d, so that's the check here
                uassert( 16823, (string)"Cannot use " + IndexNames::GEO_2DSPHERE +
                                    " index with other special index types: " + e.toString(),
                         e.isNumber() );
            }
        }
        uassert(16750, "Expect at least one geo field, spec=" + descriptor->keyPattern().toString(),
                geoFields >= 1);
    }

    // static
    BSONObj S2AccessMethod::fixSpec(const BSONObj& specObj) {
        // If the spec object has the field "2dsphereIndexVersion", validate it.  If it doesn't, add
        // {2dsphereIndexVersion: 2}, which is the default for newly-built indexes.

        BSONElement indexVersionElt = specObj[kIndexVersionFieldName];
        if (indexVersionElt.eoo()) {
            BSONObjBuilder bob;
            bob.appendElements(specObj);
            bob.append(kIndexVersionFieldName, S2_INDEX_VERSION_2);
            return bob.obj();
        }

        const int indexVersion = indexVersionElt.numberInt();
        uassert(17394,
                str::stream() << "unsupported geo index version { " << kIndexVersionFieldName
                              << " : " << indexVersionElt << " }, only support versions: ["
                              << S2_INDEX_VERSION_1 << "," << S2_INDEX_VERSION_2 << "]",
                indexVersionElt.isNumber() && (indexVersion == S2_INDEX_VERSION_2
                                               || indexVersion == S2_INDEX_VERSION_1));
        return specObj;
    }

    void S2AccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        getS2Keys(obj, _descriptor->keyPattern(), _params, keys);
    }

}  // namespace mongo
