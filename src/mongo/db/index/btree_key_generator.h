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

#pragma once

#include <vector>
#include <set>
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Internal class used by BtreeAccessMethod to generate keys for indexed documents.
     * This class is meant to be kept under the index access layer.
     */
    class BtreeKeyGenerator {
    public:
        BtreeKeyGenerator(vector<const char*> fieldNames, vector<BSONElement> fixed, bool isSparse);
        virtual ~BtreeKeyGenerator() { }

        void getKeys(const BSONObj &obj, BSONObjSet *keys) const;

        static const int ParallelArraysCode;

    protected:
        // These are used by the getKeysImpl(s) below.
        vector<const char*> _fieldNames;
        bool _isSparse;
        BSONObj _nullKey; // a full key with all fields null
        BSONObj _nullObj;     // only used for _nullElt
        BSONElement _nullElt; // jstNull
        BSONSizeTracker _sizeTracker;
    private:
        // We have V0 and V1.  Sigh.
        virtual void getKeysImpl(vector<const char*> fieldNames, vector<BSONElement> fixed,
                                 const BSONObj &obj, BSONObjSet *keys) const = 0;
        vector<BSONElement> _fixed;
    };

    class BtreeKeyGeneratorV0 : public BtreeKeyGenerator {
    public:
        BtreeKeyGeneratorV0(vector<const char*> fieldNames, vector<BSONElement> fixed,
                            bool isSparse);
        virtual ~BtreeKeyGeneratorV0() { }
        
    private:
        virtual void getKeysImpl(vector<const char*> fieldNames, vector<BSONElement> fixed,
                                 const BSONObj &obj, BSONObjSet *keys) const;
    };

    class BtreeKeyGeneratorV1 : public BtreeKeyGenerator {
    public:
        BtreeKeyGeneratorV1(vector<const char*> fieldNames, vector<BSONElement> fixed,
                            bool isSparse);
        virtual ~BtreeKeyGeneratorV1() { }

    private:
        /**
         * @param fieldNames - fields to index, may be postfixes in recursive calls
         * @param fixed - values that have already been identified for their index fields
         * @param obj - object from which keys should be extracted, based on names in fieldNames
         * @param keys - set where index keys are written
         * @param numNotFound - number of index fields that have already been identified as missing
         * @param array - array from which keys should be extracted, based on names in fieldNames
         *        If obj and array are both nonempty, obj will be one of the elements of array.
         */        
        virtual void getKeysImpl(vector<const char*> fieldNames, vector<BSONElement> fixed,
                                 const BSONObj &obj, BSONObjSet *keys) const;

        // These guys are called by getKeysImpl.
        void getKeysImplWithArray(vector<const char*> fieldNames, vector<BSONElement> fixed,
                                  const BSONObj &obj, BSONObjSet *keys, unsigned numNotFound,
                                  const BSONObj &array) const;
        /**
         * @param arrayNestedArray - set if the returned element is an array nested directly
                                     within arr.
         */
        BSONElement extractNextElement(const BSONObj &obj, const BSONObj &arr, const char *&field,
                                       bool &arrayNestedArray ) const;
        void _getKeysArrEltFixed(vector<const char*> &fieldNames, vector<BSONElement> &fixed,
                                 const BSONElement &arrEntry, BSONObjSet *keys,
                                 unsigned numNotFound, const BSONElement &arrObjElt,
                                 const set<unsigned> &arrIdxs, bool mayExpandArrayUnembedded) const;
        
        BSONObj _undefinedObj;
        BSONElement _undefinedElt;
    };

}  // namespace mongo
