// v8_db.h

/*    Copyright 2014 MongoDB Inc.
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

#include <v8.h>

#include "mongo/scripting/engine_v8-3.25.h"
#include "mongo/stdx/functional.h"

namespace mongo {

    class DBClientBase;

    /**
     * get the DBClientBase connection from JS args
     */
    boost::shared_ptr<mongo::DBClientBase> getConnection(V8Scope* scope,
        const v8::FunctionCallbackInfo<v8::Value>& args);

    // Internal Cursor
    v8::Local<v8::FunctionTemplate> getInternalCursorFunctionTemplate(V8Scope* scope);

    // Mongo constructors
    v8::Local<v8::Value> mongoConsLocal(V8Scope* scope,
                                        const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoConsExternal(V8Scope* scope,
                                           const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::FunctionTemplate> getMongoFunctionTemplate(V8Scope* scope, bool local);

    // Mongo member functions
    v8::Local<v8::Value> mongoFind(V8Scope* scope,
                                   const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoInsert(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoRemove(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoUpdate(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoAuth(V8Scope* scope,
                                   const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoLogout(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> mongoCursorFromId(V8Scope* scope,
                                           const v8::FunctionCallbackInfo<v8::Value>& args);

    // Cursor object
    v8::Local<v8::Value> internalCursorCons(V8Scope* scope,
                                            const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> internalCursorNext(V8Scope* scope,
                                            const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> internalCursorHasNext(V8Scope* scope,
                                               const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> internalCursorObjsLeftInBatch(V8Scope* scope,
        const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> internalCursorReadOnly(V8Scope* scope,
                                                const v8::FunctionCallbackInfo<v8::Value>& args);

    // BinData object
    v8::Local<v8::Value> binDataInit(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> binDataToString(V8Scope* scope,
                                         const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> binDataToBase64(V8Scope* scope,
                                         const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> binDataToHex(V8Scope* scope,
                                      const v8::FunctionCallbackInfo<v8::Value>& args);

    // NumberLong object
    long long numberLongVal(V8Scope* scope, const v8::Local<v8::Object>& it);
    v8::Local<v8::Value> numberLongInit(V8Scope* scope,
                                        const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberLongToNumber(V8Scope* scope,
                                            const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberLongValueOf(V8Scope* scope,
                                           const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberLongToString(V8Scope* scope,
                                            const v8::FunctionCallbackInfo<v8::Value>& args);

    // NumberInt object
    int numberIntVal(V8Scope* scope, const v8::Local<v8::Object>& it);
    v8::Local<v8::Value> numberIntInit(V8Scope* scope,
                                       const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberIntToNumber(V8Scope* scope,
                                           const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberIntValueOf(V8Scope* scope,
                                          const v8::FunctionCallbackInfo<v8::Value>& args);
    v8::Local<v8::Value> numberIntToString(V8Scope* scope,
                                           const v8::FunctionCallbackInfo<v8::Value>& args);

    // DBQuery object
    v8::Local<v8::Value> dbQueryInit(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);
    void dbQueryIndexAccess(::uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info);

    // db constructor
    v8::Local<v8::Value> dbInit(V8Scope* scope, const v8::FunctionCallbackInfo<v8::Value>& args);

    // collection constructor
    v8::Local<v8::Value> collectionInit(V8Scope* scope,
                                        const v8::FunctionCallbackInfo<v8::Value>& args);

    // ObjectId constructor
    v8::Local<v8::Value> objectIdInit(V8Scope* scope,
                                      const v8::FunctionCallbackInfo<v8::Value>& args);

    // DBRef constructor
    v8::Local<v8::Value> dbRefInit(V8Scope* scope,
                                   const v8::FunctionCallbackInfo<v8::Value>& args);

    // DBPointer constructor
    v8::Local<v8::Value> dbPointerInit(V8Scope* scope,
                                       const v8::FunctionCallbackInfo<v8::Value>& args);

    // Timestamp constructor
    v8::Local<v8::Value> dbTimestampInit(V8Scope* scope,
                                         const v8::FunctionCallbackInfo<v8::Value>& args);

    // UUID constructor
    v8::Local<v8::Value> uuidInit(V8Scope* scope, const v8::FunctionCallbackInfo<v8::Value>& args);

    // MD5 constructor
    v8::Local<v8::Value> md5Init(V8Scope* scope, const v8::FunctionCallbackInfo<v8::Value>& args);

    // HexData constructor
    v8::Local<v8::Value> hexDataInit(V8Scope* scope,
                                     const v8::FunctionCallbackInfo<v8::Value>& args);

    // Object.invalidForStorage()
    v8::Local<v8::Value> v8ObjectInvalidForStorage(V8Scope* scope,
                                                   const v8::FunctionCallbackInfo<v8::Value>& args);

    // Object.bsonsize()
    v8::Local<v8::Value> bsonsize(V8Scope* scope, const v8::FunctionCallbackInfo<v8::Value>& args);

    // global method
    // Accepts 2 objects, converts them to BSONObj and calls woCompare on the first against the
    // second.
    v8::Local<v8::Value> bsonWoCompare(V8Scope* scope,
                                       const v8::FunctionCallbackInfo<v8::Value>& args);

    // 'db.collection' property handlers
    void collectionGetter(v8::Local<v8::String> name,
                                           const v8::PropertyCallbackInfo<v8::Value>& info);
    void collectionSetter(v8::Local<v8::String> name, v8::Local<v8::Value> value,
                                           const v8::PropertyCallbackInfo<v8::Value>& info);

    typedef stdx::function<void (V8Scope*, const v8::Local<v8::FunctionTemplate>&)>
            V8FunctionPrototypeManipulatorFn;

    void v8RegisterMongoPrototypeManipulator(const V8FunctionPrototypeManipulatorFn& manipulator);
}

