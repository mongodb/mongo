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

#include "mongo/pch.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/json.h"
#include "mongo/tools/mongoexport_options.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_logger.h"
#include "mongo/util/options_parser/option_section.h"

using namespace mongo;

class Export : public Tool {
public:
    Export() : Tool() { }

    virtual void printHelp( ostream & out ) {
        printMongoExportHelp(&out);
    }

    // Turn every double quote character into two double quote characters
    // If hasSurroundingQuotes is true, doesn't escape the first and last
    // characters of the string, if it's false, add a double quote character
    // around the whole string.
    string csvEscape(string str, bool hasSurroundingQuotes = false) {
        size_t index = hasSurroundingQuotes ? 1 : 0;
        while (((index = str.find('"', index)) != string::npos)
               && (index < (hasSurroundingQuotes ? str.size() - 1 : str.size()))) {
            str.replace(index, 1, "\"\"");
            index += 2;
        }
        return hasSurroundingQuotes ? str : "\"" + str + "\"";
    }

    // Gets the string representation of a BSON object that can be correctly written to a CSV file
    string csvString (const BSONElement& object) {
        const char* binData; // Only used with BinData type

        switch (object.type()) {
        case MinKey:
            return "$MinKey";
        case MaxKey:
            return "$MaxKey";
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case Bool:
            return object.toString(false);
        case String:
        case Symbol:
            return csvEscape(object.toString(false, true), true);
        case Object:
            return csvEscape(object.jsonString(Strict, false));
        case Array:
            return csvEscape(object.jsonString(Strict, false));
        case BinData:
            int len;
            binData = object.binDataClean(len);
            return toHex(binData, len);
        case jstOID:
            return "ObjectID(" + object.OID().toString() + ")"; // OIDs are always 24 bytes
        case Date:
            return timeToISOString(object.Date() / 1000);
        case Timestamp:
            return csvEscape(object.jsonString(Strict, false));
        case RegEx:
            return csvEscape("/" + string(object.regex()) + "/" + string(object.regexFlags()));
        case Code:
            return csvEscape(object.toString(false));
        case CodeWScope:
            if (string(object.codeWScopeScopeDataUnsafe()) == "") {
                return csvEscape(object.toString(false));
            } else {
                return csvEscape(object.jsonString(Strict, false));
            }
        case EOO:
        case Undefined:
        case DBRef:
        case jstNULL:
            cerr << "Invalid BSON object type for CSV output: " << object.type() << endl;
            return "";
        }
        // Can never get here
        verify(false);
        return "";
    }

    int run() {
        string ns;
        ostream *outPtr = &cout;
        auto_ptr<ofstream> fileStream;
        if (mongoExportGlobalParams.outputFileSpecified && mongoExportGlobalParams.outputFile != "-") {
            size_t idx = mongoExportGlobalParams.outputFile.rfind("/");
            if ( idx != string::npos ) {
                string dir = mongoExportGlobalParams.outputFile.substr( 0 , idx + 1 );
                boost::filesystem::create_directories( dir );
            }
            ofstream * s = new ofstream(mongoExportGlobalParams.outputFile.c_str(), ios_base::out);
            fileStream.reset( s );
            outPtr = s;
            if ( ! s->good() ) {
                cerr << "couldn't open [" << mongoExportGlobalParams.outputFile << "]" << endl;
                return -1;
            }
        }
        ostream &out = *outPtr;

        BSONObj * fieldsToReturn = 0;
        BSONObj realFieldsToReturn;

        try {
            ns = getNS();
        }
        catch (...) {
            printHelp(cerr);
            return 1;
        }

        if (toolGlobalParams.fieldsSpecified || mongoExportGlobalParams.csv) {
            
            // we can't use just toolGlobalParams.fields since we support everything getFieldDotted
            // does
            
            set<string> seen;
            BSONObjBuilder b;
            
            for (std::vector<std::string>::iterator i = toolGlobalParams.fields.begin();
                 i != toolGlobalParams.fields.end(); i++) {
                std::string f = str::before(*i, '.');
                if ( seen.insert( f ).second )
                    b.append( f , 1 );
            }
            
            realFieldsToReturn = b.obj();
            fieldsToReturn = &realFieldsToReturn;
        }
        
        
        if (mongoExportGlobalParams.csv && !toolGlobalParams.fieldsSpecified) {
            cerr << "csv mode requires a field list" << endl;
            return -1;
        }

        Query q(mongoExportGlobalParams.query);

        if (mongoExportGlobalParams.snapShotQuery) {
            q.snapshot();
        }

        auto_ptr<DBClientCursor> cursor = conn().query(ns.c_str(), q,
                mongoExportGlobalParams.limit, mongoExportGlobalParams.skip, fieldsToReturn,
                (mongoExportGlobalParams.slaveOk ? QueryOption_SlaveOk : 0) |
                QueryOption_NoCursorTimeout);

        if (mongoExportGlobalParams.csv) {
            for (std::vector<std::string>::iterator i = toolGlobalParams.fields.begin();
                 i != toolGlobalParams.fields.end(); i++) {
                if (i != toolGlobalParams.fields.begin())
                    out << ",";
                out << *i;
            }
            out << endl;
        }

        if (mongoExportGlobalParams.jsonArray)
            out << '[';

        long long num = 0;
        while ( cursor->more() ) {
            num++;
            BSONObj obj = cursor->next();
            if (mongoExportGlobalParams.csv) {
                for (std::vector<std::string>::iterator i = toolGlobalParams.fields.begin();
                     i != toolGlobalParams.fields.end(); i++) {
                    if (i != toolGlobalParams.fields.begin())
                        out << ",";
                    const BSONElement & e = obj.getFieldDotted(i->c_str());
                    if ( ! e.eoo() ) {
                        out << csvString(e);
                    }
                }
                out << endl;
            }
            else {
                if (mongoExportGlobalParams.jsonArray && num != 1)
                    out << ',';

                out << obj.jsonString();

                if (!mongoExportGlobalParams.jsonArray)
                    out << endl;
            }
        }

        if (mongoExportGlobalParams.jsonArray)
            out << ']' << endl;

        toolInfoOutput() << "exported " << num << " records" << endl;

        return 0;
    }
};

REGISTER_MONGO_TOOL(Export);
