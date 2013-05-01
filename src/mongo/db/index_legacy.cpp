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
*/

#include "mongo/db/index_legacy.h"

#include "mongo/db/client.h"
#include "mongo/db/fts/fts_enabled.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    // static
    BSONObj IndexLegacy::adjustIndexSpecObject(const BSONObj& obj) {
        string pluginName = IndexNames::findPluginName(obj.getObjectField("key"));

        if (IndexNames::TEXT == pluginName || IndexNames::TEXT_INTERNAL == pluginName) {
            StringData desc = cc().desc();
            if (desc.find("conn") == 0) {
                // this is to make sure we only complain for users
                // if you do get a text index created an a primary
                // want it to index on the secondary as well
                massert(16811, "text search not enabled", fts::isTextSearchEnabled() );
            }
            return fts::FTSSpec::fixSpec(obj);
        }

        return obj;
    }

    // static
    BSONObj IndexLegacy::getMissingField(const BSONObj& infoObj) {
        if (IndexNames::HASHED == CatalogHack::getAccessMethodName(infoObj.getObjectField("key"))) {
            int hashVersion = infoObj["hashVersion"].numberInt();
            HashSeed seed = infoObj["seed"].numberInt();

            // Explicit null valued fields and missing fields are both represented in hashed indexes
            // using the hash value of the null BSONElement.  This is partly for historical reasons
            // (hash of null was used in the initial release of hashed indexes and changing would
            // alter the data format).  Additionally, in certain places the hashed index code and
            // the index bound calculation code assume null and missing are indexed identically.
            BSONObj nullObj = BSON("" << BSONNULL);
            return BSON("" << HashAccessMethod::makeSingleKey(nullObj.firstElement(), seed,
                                                              hashVersion));
        }
        else {
            BSONObjBuilder b;
            b.appendNull("");
            return b.obj();
        }
    }

    // static
    void IndexLegacy::postBuildHook(NamespaceDetails* tableToIndex, const IndexDetails& idx) {
        // If it's an FTS index, we want to set the power of 2 flag.
        string pluginName = CatalogHack::getAccessMethodName(idx.keyPattern());
        if (IndexNames::TEXT == pluginName || IndexNames::TEXT_INTERNAL == pluginName) {
            if (tableToIndex->setUserFlag(NamespaceDetails::Flag_UsePowerOf2Sizes)) {
                tableToIndex->syncUserFlags(idx.parentNS());
            }
        }
    }

}  // namespace mongo
