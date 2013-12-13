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

#include "mongo/db/index_legacy.h"

#include "mongo/db/client.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    // static
    BSONObj IndexLegacy::adjustIndexSpecObject(const BSONObj& obj) {
        string pluginName = IndexNames::findPluginName(obj.getObjectField("key"));

        if (IndexNames::TEXT == pluginName) {
            return fts::FTSSpec::fixSpec(obj);
        }

        return obj;
    }

    // static
    BSONObj IndexLegacy::getMissingField(Collection* collection, const BSONObj& infoObj) {
        BSONObj keyPattern = infoObj.getObjectField( "key" );
        string accessMethodName;
        if ( collection )
            accessMethodName = collection->getIndexCatalog()->getAccessMethodName(keyPattern);
        else
            accessMethodName = IndexNames::findPluginName(keyPattern);

        if (IndexNames::HASHED == accessMethodName ) {
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
    void IndexLegacy::postBuildHook(Collection* collection, const BSONObj& keyPattern) {
        // If it's an FTS index, we want to set the power of 2 flag.
        string pluginName = collection->getIndexCatalog()->getAccessMethodName(keyPattern);
        if (IndexNames::TEXT == pluginName) {
            NamespaceDetails* nsd = collection->details();
            if (nsd->setUserFlag(NamespaceDetails::Flag_UsePowerOf2Sizes)) {
                nsd->syncUserFlags(collection->ns().ns());
            }
        }
    }

}  // namespace mongo
