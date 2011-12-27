// index_key.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include "../pch.h"
#include "diskloc.h"
#include "jsobj.h"
#include <map>

namespace mongo {

    extern const int DefaultIndexVersionNumber;

    const int ParallelArraysCode = 10088;
    
    class Cursor;
    class IndexSpec;
    class IndexType; // TODO: this name sucks
    class IndexPlugin;
    class IndexDetails;

    enum IndexSuitability { USELESS = 0 , HELPFUL = 1 , OPTIMAL = 2 };

    /**
     * this represents an instance of a index plugin
     * done this way so parsing, etc... can be cached
     * so if there is a FTS IndexPlugin, for each index using FTS
     * there will be 1 of these, and it can have things pre-parsed, etc...
     */
    class IndexType : boost::noncopyable {
    public:
        IndexType( const IndexPlugin * plugin , const IndexSpec * spec );
        virtual ~IndexType();

        virtual void getKeys( const BSONObj &obj, BSONObjSet &keys ) const = 0;
        virtual shared_ptr<Cursor> newCursor( const BSONObj& query , const BSONObj& order , int numWanted ) const = 0;

        /** optional op : changes query to match what's in the index */
        virtual BSONObj fixKey( const BSONObj& in ) { return in; }

        /** optional op : compare 2 objects with regards to this index */
        virtual int compare( const BSONObj& l , const BSONObj& r ) const;

        /** @return plugin */
        const IndexPlugin * getPlugin() const { return _plugin; }

        const BSONObj& keyPattern() const;

        virtual IndexSuitability suitability( const BSONObj& query , const BSONObj& order ) const ;

        virtual bool scanAndOrderRequired( const BSONObj& query , const BSONObj& order ) const ;

    protected:
        const IndexPlugin * _plugin;
        const IndexSpec * _spec;
    };

    /**
     * this represents a plugin
     * a plugin could be something like full text search, sparse index, etc...
     * 1 of these exists per type of index per server
     * 1 IndexType is created per index using this plugin
     */
    class IndexPlugin : boost::noncopyable {
    public:
        IndexPlugin( const string& name );
        virtual ~IndexPlugin() {}

        virtual IndexType* generate( const IndexSpec * spec ) const = 0;

        string getName() const { return _name; }

        /**
         * @return new keyPattern
         * if nothing changes, should return keyPattern
         */
        virtual BSONObj adjustIndexSpec( const BSONObj& spec ) const { return spec; }

        // ------- static below -------

        static IndexPlugin* get( const string& name ) {
            if ( ! _plugins )
                return 0;
            map<string,IndexPlugin*>::iterator i = _plugins->find( name );
            if ( i == _plugins->end() )
                return 0;
            return i->second;
        }

        /**
         * @param keyPattern { x : "fts" }
         * @return "" or the name
         */
        static string findPluginName( const BSONObj& keyPattern );

    private:
        string _name;
        static map<string,IndexPlugin*> * _plugins;
    };

    /* precomputed details about an index, used for inserting keys on updates
       stored/cached in NamespaceDetailsTransient, or can be used standalone
       */
    class IndexSpec {
    public:
        BSONObj keyPattern; // e.g., { name : 1 }
        BSONObj info; // this is the same as IndexDetails::info.obj()

        IndexSpec()
            : _details(0) , _finishedInit(false) {
        }

        explicit IndexSpec( const BSONObj& k , const BSONObj& m = BSONObj() )
            : keyPattern(k) , info(m) , _details(0) , _finishedInit(false) {
            _init();
        }

        /**
           this is a DiscLoc of an IndexDetails info
           should have a key field
         */
        explicit IndexSpec( const DiskLoc& loc ) {
            reset( loc );
        }

        void reset( const BSONObj& info );
        void reset( const DiskLoc& infoLoc ) { reset(infoLoc.obj()); }
        void reset( const IndexDetails * details );

        void getKeys( const BSONObj &obj, BSONObjSet &keys ) const;

        BSONElement missingField() const { return _nullElt; }

        string getTypeName() const {
            if ( _indexType.get() )
                return _indexType->getPlugin()->getName();
            return "";
        }

        IndexType* getType() const {
            return _indexType.get();
        }

        const IndexDetails * getDetails() const {
            return _details;
        }

        IndexSuitability suitability( const BSONObj& query , const BSONObj& order ) const ;

    protected:

        int indexVersion() const;
        
        IndexSuitability _suitability( const BSONObj& query , const BSONObj& order ) const ;

        BSONSizeTracker _sizeTracker;
        vector<const char*> _fieldNames;
        vector<BSONElement> _fixed;

        BSONObj _nullKey; // a full key with all fields null
        BSONObj _nullObj; // only used for _nullElt
        BSONElement _nullElt; // jstNull

        BSONObj _undefinedObj; // only used for _undefinedElt
        BSONElement _undefinedElt; // undefined

        int _nFields; // number of fields in the index
        bool _sparse; // if the index is sparse
        shared_ptr<IndexType> _indexType;
        const IndexDetails * _details;

        void _init();

        friend class IndexType;
        friend class KeyGeneratorV0;
        friend class KeyGeneratorV1;
    public:
        bool _finishedInit;
    };


} // namespace mongo
