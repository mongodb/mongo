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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/s/bson_serializable.h"

namespace mongo {

    /**
     * This class represents the layout and content of a TypeExplain runCommand,
     * the response side.
     */
    class TypeExplain : public BSONSerializable {
        MONGO_DISALLOW_COPYING(TypeExplain);
    public:

        //
        // schema declarations
        //

        static const BSONField<std::vector<TypeExplain*> > clauses;
        static const BSONField<std::string> cursor;
        static const BSONField<bool> isMultiKey;
        static const BSONField<long long> n;
        static const BSONField<long long> nScannedObjects;
        static const BSONField<long long> nScanned;
        static const BSONField<long long> nScannedObjectsAllPlans;
        static const BSONField<long long> nScannedAllPlans;
        static const BSONField<bool> scanAndOrder;
        static const BSONField<bool> indexOnly;
        static const BSONField<long long> nYields;
        static const BSONField<long long> nChunkSkips;
        static const BSONField<long long> millis;
        static const BSONField<BSONObj> indexBounds;
        static const BSONField<std::vector<TypeExplain*> > allPlans;
        static const BSONField<TypeExplain*> oldPlan;
        static const BSONField<std::string> server;

        //
        // construction / destruction
        //

        TypeExplain();
        virtual ~TypeExplain();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(TypeExplain* other) const;

        //
        // bson serializable interface implementation
        //

        virtual bool isValid(std::string* errMsg) const;
        virtual BSONObj toBSON() const;
        virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
        virtual void clear();
        virtual std::string toString() const;

        //
        // individual field accessors
        //

        void setClauses(const std::vector<TypeExplain*>& clauses);
        void addToClauses(TypeExplain* clauses);
        void unsetClauses();
        bool isClausesSet() const;
        size_t sizeClauses() const;
        const std::vector<TypeExplain*>& getClauses() const;
        const TypeExplain* getClausesAt(size_t pos) const;

        void setCursor(const StringData& cursor);
        void unsetCursor();
        bool isCursorSet() const;
        const std::string& getCursor() const;

        void setIsMultiKey(bool isMultiKey);
        void unsetIsMultiKey();
        bool isIsMultiKeySet() const;
        bool getIsMultiKey() const;

        void setN(long long n);
        void unsetN();
        bool isNSet() const;
        long long getN() const;

        void setNScannedObjects(long long nScannedObjects);
        void unsetNScannedObjects();
        bool isNScannedObjectsSet() const;
        long long getNScannedObjects() const;

        void setNScanned(long long nScanned);
        void unsetNScanned();
        bool isNScannedSet() const;
        long long getNScanned() const;

        void setNScannedObjectsAllPlans(long long nScannedObjectsAllPlans);
        void unsetNScannedObjectsAllPlans();
        bool isNScannedObjectsAllPlansSet() const;
        long long getNScannedObjectsAllPlans() const;

        void setNScannedAllPlans(long long nScannedAllPlans);
        void unsetNScannedAllPlans();
        bool isNScannedAllPlansSet() const;
        long long getNScannedAllPlans() const;

        void setScanAndOrder(bool scanAndOrder);
        void unsetScanAndOrder();
        bool isScanAndOrderSet() const;
        bool getScanAndOrder() const;

        void setIndexOnly(bool indexOnly);
        void unsetIndexOnly();
        bool isIndexOnlySet() const;
        bool getIndexOnly() const;

        void setNYields(long long nYields);
        void unsetNYields();
        bool isNYieldsSet() const;
        long long getNYields() const;

        void setNChunkSkips(long long nChunkSkips);
        void unsetNChunkSkips();
        bool isNChunkSkipsSet() const;
        long long getNChunkSkips() const;

        void setMillis(long long millis);
        void unsetMillis();
        bool isMillisSet() const;
        long long getMillis() const;

        void setIndexBounds(const BSONObj& indexBounds);
        void unsetIndexBounds();
        bool isIndexBoundsSet() const;
        const BSONObj& getIndexBounds() const;

        void setAllPlans(const std::vector<TypeExplain*>& allPlans);
        void addToAllPlans(TypeExplain* allPlans);
        void unsetAllPlans();
        bool isAllPlansSet() const;
        size_t sizeAllPlans() const;
        const std::vector<TypeExplain*>& getAllPlans() const;
        const TypeExplain* getAllPlansAt(size_t pos) const;

        void setOldPlan(TypeExplain* oldPlan);
        void unsetOldPlan();
        bool isOldPlanSet() const;
        const TypeExplain* getOldPlan() const;

        void setServer(const StringData& server);
        void unsetServer();
        bool isServerSet() const;
        const std::string& getServer() const;

    private:
        // Convention: (M)andatory, (O)ptional

        // (O)  explain for branches on a $or query
        boost::scoped_ptr<std::vector<TypeExplain*> >_clauses;

        // (O)  type and name of the cursor used on the leaf stage
        std::string _cursor;
        bool _isCursorSet;

        // (O)  type and name of the cursor used on the leaf stage
        bool _isMultiKey;
        bool _isIsMultiKeySet;

        // (M)  number of documents returned by the query
        long long _n;
        bool _isNSet;

        // (M)  number of documents fetched entirely from the disk
        long long _nScannedObjects;
        bool _isNScannedObjectsSet;

        // (M)  number of entries retrieved either from an index or collection
        long long _nScanned;
        bool _isNScannedSet;

        // (O)  number of documents fetched entirely from the disk across all plans
        long long _nScannedObjectsAllPlans;
        bool _isNScannedObjectsAllPlansSet;

        // (O)  number of entries retrieved either from an index or collection across all plans
        long long _nScannedAllPlans;
        bool _isNScannedAllPlansSet;

        // (O)  whether this plan involved sorting
        bool _scanAndOrder;
        bool _isScanAndOrderSet;

        // (O)  number of entries retrieved either from an index or collection across all plans
        bool _indexOnly;
        bool _isIndexOnlySet;

        // (O)  number times this plan released and reacquired its lock
        long long _nYields;
        bool _isNYieldsSet;

        // (O)  number times this plan skipped over migrated data
        long long _nChunkSkips;
        bool _isNChunkSkipsSet;

        // (O)  elapsed time this plan took running, in milliseconds
        long long _millis;
        bool _isMillisSet;

        // (O)  keys used to seek in and out of an index
        BSONObj _indexBounds;
        bool _isIndexBoundsSet;

        // (O)  alternative plans considered
        boost::scoped_ptr<std::vector<TypeExplain*> > _allPlans;

        // (O)  cached plan for this query
        boost::scoped_ptr<TypeExplain> _oldPlan;

        // (O)  server's host:port against which the query ran
        std::string _server;
        bool _isServerSet;
    };

} // namespace mongo
