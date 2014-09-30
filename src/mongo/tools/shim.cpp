// shim.cpp

/**
*    Copyright (C) 2014 MongoDB, INC.
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

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>
#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/tools/mongoshim_options.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_logger.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/scopeguard.h"

using std::auto_ptr;
using std::ios_base;
using std::ofstream;
using std::string;
using std::vector;

using namespace mongo;



/**
 * Mongoshim mode handlers
 */
class ShimHandler {
public:
    virtual ~ShimHandler() { }

    /**
     * Returns true if this mode requires an output stream
     */
    virtual bool requiresOutputStream() const = 0;

    /**
     * If true, processes documents read from input in gotObject() - doRun() will not be called.
     */
    virtual bool acceptsInputDocuments() const = 0;

    /**
     * Process input document.
     * Results may be written to output stream if provided.
     */
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const = 0;

    /**
     * Generate results for this mode and write results to output stream if applicable.
     * During processing, only one of handleSingleInputDocument() or generateOutputDocuments()
     * will be called as determined by the result of acceptsInputDocuments().
     */
    virtual void generateOutputDocuments(std::ostream* out) const = 0;
};



/**
 * Find mode.
 */
class FindShimHandler : public ShimHandler {
public:
    FindShimHandler(DBClientBase& connection, const string& ns)
        : _connection(connection), _ns(ns) { }

    virtual bool requiresOutputStream() const { return true; }
    virtual bool acceptsInputDocuments() const { return false; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const { }
    virtual void generateOutputDocuments(std::ostream* out) const {
        invariant(out);
        Query q(mongoShimGlobalParams.query);
        if (mongoShimGlobalParams.sort != "") {
            BSONObj sortSpec = mongo::fromjson(mongoShimGlobalParams.sort);
            q.sort(sortSpec);
        }

        auto_ptr<DBClientCursor> cursor =
            _connection.query(_ns, q, mongoShimGlobalParams.limit, mongoShimGlobalParams.skip,
                              NULL, 0, QueryOption_NoCursorTimeout);

        while ( cursor->more() ) {
            BSONObj obj = cursor->next();
            out->write( obj.objdata(), obj.objsize() );
        }
    }

private:
    DBClientBase& _connection;
    string _ns;
};



/**
 * Insert mode.
 */
class InsertShimHandler : public ShimHandler {
public:
    InsertShimHandler(DBClientBase& connection, const string& ns)
        : _connection(connection), _ns(ns) {
        if (mongoShimGlobalParams.drop) {
            invariant(!_ns.empty());
            _connection.dropCollection(_ns);
        }
    }

    virtual bool requiresOutputStream() const { return false; }
    virtual bool acceptsInputDocuments() const { return true; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const {
        _connection.insert(_ns, obj);
    }
    virtual void generateOutputDocuments(std::ostream* out) const { }

private:
    DBClientBase& _connection;
    string _ns;
};



/**
 * Upsert mode.
 */
class UpsertShimHandler : public ShimHandler {
public:
    UpsertShimHandler(DBClientBase& connection, const string& ns)
        : _connection(connection), _ns(ns) { }

    virtual bool requiresOutputStream() const { return false; }
    virtual bool acceptsInputDocuments() const { return true; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const {
        BSONObjBuilder b;
        invariant(!mongoShimGlobalParams.upsertFields.empty());
        for (vector<string>::const_iterator it = mongoShimGlobalParams.upsertFields.begin(),
             end = mongoShimGlobalParams.upsertFields.end(); it != end; ++it) {
            BSONElement e = obj.getFieldDotted(it->c_str());
            // If we cannot construct a valid query using the provided upsertFields,
            // insert the object and skip the rest of the fields.
            if (e.eoo()) {
                _connection.insert(_ns, obj);
                return;
            }
            b.appendAs(e, *it);
        }
        Query query(b.obj());
        bool upsert = true;
        bool multi = false;
        _connection.update(_ns, query, obj, upsert, multi);
    }
    virtual void generateOutputDocuments(std::ostream* out) const { }

private:
    DBClientBase& _connection;
    string _ns;
};



/**
 * Remove mode.
 */
class RemoveShimHandler : public ShimHandler {
public:
    RemoveShimHandler(DBClientBase& connection, const string& ns)
        : _connection(connection), _ns(ns) { }

    virtual bool requiresOutputStream() const { return false; }
    virtual bool acceptsInputDocuments() const { return false; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const { }
    virtual void generateOutputDocuments(std::ostream* out) const {
        // Removes all documents matching query
        bool justOne = false;
        _connection.remove(_ns, mongoShimGlobalParams.query, justOne);
    }

private:
    DBClientBase& _connection;
    string _ns;
};



/**
 * Repair mode.
 * Writes valid objects in collection to output.
 */
class RepairShimHandler : public ShimHandler {
public:
    RepairShimHandler(DBClientBase& connection, const string& ns)
        : _connection(connection), _ns(ns) { }

    virtual bool requiresOutputStream() const { return true; }
    virtual bool acceptsInputDocuments() const { return false; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const { }
    virtual void generateOutputDocuments(std::ostream* out) const {
        invariant(out);
        toolInfoLog() << "going to try to recover data from: " << _ns << std::endl;
        OperationContextImpl txn;
        Client::WriteContext cx(&txn, toolGlobalParams.db);

        Database* db = dbHolder().get(&txn, toolGlobalParams.db);
        Collection* collection = db->getCollection(&txn, _ns);

        if (!collection) {
            toolError() << "Collection does not exist: " << toolGlobalParams.coll << std::endl;
            return;
        }

        toolInfoLog() << "nrecords: " << collection->numRecords(&txn)
                      << " datasize: " << collection->dataSize(&txn);
        try {
            boost::scoped_ptr<RecordIterator> iter(
                collection->getRecordStore()->getIteratorForRepair(&txn));
            for (DiskLoc currLoc = iter->getNext(); !currLoc.isNull(); currLoc = iter->getNext()) {
                if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                    toolInfoLog() << currLoc;
                }

                BSONObj obj;
                try {
                    obj = collection->docFor(&txn, currLoc);

                    // If this is a corrupted object, just skip it, but do not abort the scan
                    //
                    if (!obj.valid()) {
                        continue;
                    }

                    if (logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1))) {
                        toolInfoLog() << obj;
                    }

                    // Write valid object to output stream.
                    out->write(obj.objdata(), obj.objsize());
                }
                catch (std::exception& ex) {
                    toolError() << "found invalid document @ " << currLoc << " " << ex.what();
                    if ( ! obj.isEmpty() ) {
                        try {
                            toolError() << "first element: " << obj.firstElement();
                        }
                        catch ( std::exception& ) {
                            toolError() << "unable to log invalid document @ " << currLoc;
                        }
                    }
                }
            }
        }
        catch (DBException& e) {
            toolError() << "ERROR recovering: " << _ns << " " << e.toString();
        }
        cx.commit();
    }

private:
    DBClientBase& _connection;
    string _ns;
};



/**
 * Command mode.
 */
class CommandShimHandler : public ShimHandler {
public:
    CommandShimHandler(DBClientBase& connection)
        : _connection(connection) { }

    virtual bool requiresOutputStream() const { return true; }
    virtual bool acceptsInputDocuments() const { return true; }
    virtual void handleSingleInputDocument(const BSONObj& obj, std::ostream* out) const {
        invariant(out);
        // Every document read from input is a separate command object.
        BSONObj res;
        bool ok = _connection.runCommand(toolGlobalParams.db, obj, res);
        if (!ok) {
            toolError() << "Failed to run command " << obj << ": " << res;
        }
        invariant(res.isValid());
        out->write(res.objdata(), res.objsize());
    }
    virtual void generateOutputDocuments(std::ostream* out) const { }

private:
    DBClientBase& _connection;
};



class Shim : public BSONTool {
public:
    Shim() : BSONTool() { }

    virtual void printHelp( ostream & out ) {
        printMongoShimHelp(&out);
    }

    virtual void gotObject(const BSONObj& obj, std::ostream* out) {
        invariant(_shimHandler.get());
        _shimHandler->handleSingleInputDocument(obj, out);
    }

    int doRun() {
        // Flush stdout before returning from doRun().
        // XXX: This seems to be an issue under RHEL55 but not under other OSes or more recent
        //      versions of Linux
        ON_BLOCK_EXIT(&std::ostream::flush, &cout);

        // getNS() could throw an exception.
        string ns;
        try {
            if (mongoShimGlobalParams.mode != ShimMode::kCommand) {
                ns = getNS();
            }
        }
        catch (...) {
            printHelp(cerr);
            return EXIT_FAILURE;
        }

        switch (mongoShimGlobalParams.mode) {
        case ShimMode::kFind:
            _shimHandler.reset(new FindShimHandler(conn(), ns));
            break;
        case ShimMode::kInsert:
            _shimHandler.reset(new InsertShimHandler(conn(), ns));
            break;
        case ShimMode::kUpsert:
            _shimHandler.reset(new UpsertShimHandler(conn(), ns));
            break;
        case ShimMode::kRemove:
            _shimHandler.reset(new RemoveShimHandler(conn(), ns));
            break;
        case ShimMode::kRepair:
            _shimHandler.reset(new RepairShimHandler(conn(), ns));
            break;
        case ShimMode::kCommand:
            _shimHandler.reset(new CommandShimHandler(conn()));
            break;
        case ShimMode::kNumShimModes:
            invariant(false);
        }

        // Initialize output stream if handler needs it.
        ostream *out = NULL;
        auto_ptr<ofstream> fileStream;
        if (_shimHandler->requiresOutputStream()) {
            fileStream = _createOutputFile();
            if (fileStream.get()) {
                if (!fileStream->good()) {
                    toolError() << "couldn't open [" << mongoShimGlobalParams.outputFile << "]";
                    return EXIT_FAILURE;
                }
                out = fileStream.get();
            }
            else {
                // Write results to stdout by default.
                out = &cout;
            }
        }

        // Skip doRun() if handler needs to read documents from input.
        // The handler may still write results to output stream.
        if (_shimHandler->acceptsInputDocuments()) {
            // --inputDocuments and --in are used primarily for testing.
            if (!mongoShimGlobalParams.inputDocuments.isEmpty()) {
                BSONElement firstElement = mongoShimGlobalParams.inputDocuments.firstElement();
                if (firstElement.type() != Array) {
                    toolError() << "first element of --inputDocuments has to be an array: "
                                << firstElement;
                    return EXIT_FAILURE;
                }
                BSONObjIterator i(firstElement.Obj());
                while ( i.more() ) {
                   BSONElement e = i.next();
                   if (!e.isABSONObj()) {
                       toolError() << "skipping non-object in input documents: " << e;
                       continue;
                   }
                   gotObject(e.Obj(), out);
                }
            }
            else if (mongoShimGlobalParams.inputFileSpecified) {
                processFile(mongoShimGlobalParams.inputFile, out);
            }
            else {
                processFile("-", out);
            }
        }
        else {
            // 'out' may be NULL if not required by handler (eg. "remove" mode).
            _shimHandler->generateOutputDocuments(out);
        }

        return EXIT_SUCCESS;
    }

private:

    /**
     * Returns a valid filestream if output file is specified and is not "-".
     */
    auto_ptr<ofstream> _createOutputFile() {
        auto_ptr<ofstream> fileStream;
        if (mongoShimGlobalParams.outputFileSpecified && mongoShimGlobalParams.outputFile != "-") {
            size_t idx = mongoShimGlobalParams.outputFile.rfind("/");
            if (idx != string::npos) {
                string dir = mongoShimGlobalParams.outputFile.substr(0 , idx + 1);
                boost::filesystem::create_directories(dir);
            }
            fileStream.reset(new ofstream(mongoShimGlobalParams.outputFile.c_str(),
                                          ios_base::out | ios_base::binary));
        }
        return fileStream;
    }

    auto_ptr<ShimHandler> _shimHandler;
};

REGISTER_MONGO_TOOL(Shim);
