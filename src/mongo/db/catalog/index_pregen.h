// index_pregen.h

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/key_generator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"

/**
 * This entire thing goes away with document level locking
 */
namespace mongo {

    class Collection;
    class IndexCatalogEntry;

    /**
     * One per index for pregenerated keys
     */
    struct PregeneratedKeysOnIndex {
        PregeneratedKeysOnIndex() {}
        PregeneratedKeysOnIndex( const PregeneratedKeysOnIndex& other ) {
            invariant( keys.empty() );
            generator = other.generator;
        }
        boost::shared_ptr<KeyGenerator> generator;
        BSONObjSet keys;
    };

    /**
     * one per document
     * not thread safe
     */
    class PregeneratedKeys {
    public:
        PregeneratedKeys(){}
        PregeneratedKeys( const PregeneratedKeys& other ){
            // we need the copy construct for insertion into map
            // we don't want to actually copy data though
            // so we fail in that case
            invariant( _indexes.empty() );
        }
        const PregeneratedKeysOnIndex* get( IndexCatalogEntry* entry ) const;

        void gen( const BSONObj& obj,
                  IndexCatalogEntry* entry,
                  const boost::shared_ptr<KeyGenerator>& generator );

        void clear() { _indexes.clear(); }
    private:
        typedef std::map<IndexCatalogEntry*,PregeneratedKeysOnIndex> Map;
        Map _indexes;
    };

    /**
     * this is a singleten
     */
    class GeneratorHolder {
    public:
        GeneratorHolder();

        /**
         * @return if we have a cache entry for this
         */
        bool prepare( const StringData& ns,
                      const BSONObj& obj,
                      PregeneratedKeys* out );

        void reset( const Collection* aCollection );

        void dropped( const std::string& ns );

        void droppedDatabase( const std::string& ns );

        static GeneratorHolder* getInstance();

    private:
        struct MyIndex {
            IndexCatalogEntry* entry; // cannot use as a pointer, just a number
            boost::shared_ptr<KeyGenerator> generator; // safe to use
        };

        struct MyCollection {
            std::string ns;
            std::vector<MyIndex> indexes;
        };

        typedef std::map< string,boost::shared_ptr<MyCollection> > Collections; // map from namespace to the indexes
        SimpleMutex _collectionsLock; // for modifying the map structure itself
        Collections _collections;
    };
}
