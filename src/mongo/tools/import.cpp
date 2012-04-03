// import.cpp

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

#include "pch.h"
#include "db/json.h"

#include "tool.h"
#include "../util/text.h"

#include <fstream>
#include <iostream>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>

using namespace mongo;
using std::string;
using std::stringstream;

namespace po = boost::program_options;

class Import : public Tool {

    enum Type { JSON , CSV , TSV };
    Type _type;

    const char * _sep;
    bool _ignoreBlanks;
    bool _headerLine;
    bool _upsert;
    bool _doimport;
    bool _jsonArray;
    vector<string> _upsertFields;
    static const int BUF_SIZE = 1024 * 1024 * 16;

    void csvTokenizeRow(const string& row, vector<string>& tokens) {
        bool inQuotes = false;
        bool prevWasQuote = false;
        bool tokenQuoted = false;
        string curtoken = "";
        for (string::const_iterator it = row.begin(); it != row.end(); ++it) {
            char element = *it;
            if (element == '"') {
                if (!inQuotes) {
                    inQuotes = true;
                    tokenQuoted = true;
                    curtoken = "";
                } else {
                    if (prevWasQuote) {
                        curtoken += "\"";
                        prevWasQuote = false;
                    } else {
                        prevWasQuote = true;
                    }
                }
            } else {
                if (inQuotes && prevWasQuote) {
                    inQuotes = false;
                    prevWasQuote = false;
                    tokens.push_back(curtoken);
                }

                if (element == ',' && !inQuotes) {
                    if (!tokenQuoted) { // If token was quoted, it's already been added
                        boost::trim(curtoken);
                        tokens.push_back(curtoken);
                    }
                    curtoken = "";
                    tokenQuoted = false;
                } else {
                    curtoken += element;
                }
            }
        }
        if (!tokenQuoted || (inQuotes && prevWasQuote)) {
            boost::trim(curtoken);
            tokens.push_back(curtoken);
        }
    }

    void _append( BSONObjBuilder& b , const string& fieldName , const string& data ) {
        if ( _ignoreBlanks && data.size() == 0 )
            return;

        if ( b.appendAsNumber( fieldName , data ) )
            return;

        // TODO: other types?
        b.append ( fieldName , data );
    }

    /*
     * Reads one line from in into buf.
     * Returns the number of bytes that should be skipped - the caller should
     * increment buf by this amount.
     */
    int getLine(istream* in, char* buf) {
        if (_jsonArray) {
            in->read(buf, BUF_SIZE);
            uassert(13295, "JSONArray file too large", (in->rdstate() & ios_base::eofbit));
            buf[ in->gcount() ] = '\0';
        }
        else {
            in->getline( buf , BUF_SIZE );
            log(1) << "got line:" << buf << endl;
        }
        uassert( 10263 ,  "unknown error reading file" ,
                 (!(in->rdstate() & ios_base::badbit)) &&
                 (!(in->rdstate() & ios_base::failbit) || (in->rdstate() & ios_base::eofbit)) );

        int numBytesSkipped = 0;
        if (strncmp("\xEF\xBB\xBF", buf, 3) == 0) { // UTF-8 BOM (notepad is stupid)
            buf += 3;
            numBytesSkipped += 3;
        }

        uassert(13289, "Invalid UTF8 character detected", isValidUTF8(buf));
        return numBytesSkipped;
    }

    /*
     * Parses a BSON object out of a JSON array.
     * Returns number of bytes processed on success and -1 on failure.
     */
    int parseJSONArray(char* buf, BSONObj& o) {
        int len = 0;
        while (buf[0] != '{' && buf[0] != '\0') {
            len++;
            buf++;
        }
        if (buf[0] == '\0')
            return -1;

        int jslen;
        try {
            o = fromjson(buf, &jslen);
        } catch ( MsgAssertionException& e ) {
            uasserted(13293, string("BSON representation of supplied JSON array is too large: ") + e.what());
        }
        len += jslen;

        return len;
    }

    /*
     * Parses one object from the input file.  This usually corresponds to one line in the input
     * file, unless the file is a CSV and contains a newline within a quoted string entry.
     * Returns a true if a BSONObj was successfully created and false if not.
     */
    bool parseRow(istream* in, BSONObj& o, int& numBytesRead) {
        boost::scoped_array<char> buffer(new char[BUF_SIZE+2]);
        char* line = buffer.get();

        numBytesRead = getLine(in, line);
        line += numBytesRead;

        if (line[0] == '\0') {
            return false;
        }
        numBytesRead += strlen( line );

        if (_type == JSON) {
            // Strip out trailing whitespace
            char * end = ( line + strlen( line ) ) - 1;
            while ( end >= line && isspace(*end) ) {
                *end = 0;
                end--;
            }
            try {
                o = fromjson( line );
            } catch ( MsgAssertionException& e ) {
                uasserted(13504, string("BSON representation of supplied JSON is too large: ") + e.what());
            }
            return true;
        }

        vector<string> tokens;
        if (_type == CSV) {
            string row;
            bool inside_quotes = false;
            size_t last_quote = 0;
            while (true) {
                string lineStr(line);
                // Deal with line breaks in quoted strings
                last_quote = lineStr.find_first_of('"');
                while (last_quote != string::npos) {
                    inside_quotes = !inside_quotes;
                    last_quote = lineStr.find_first_of('"', last_quote+1);
                }

                row.append(lineStr);

                if (inside_quotes) {
                    row.append("\n");
                    int num = getLine(in, line);
                    line += num;
                    numBytesRead += num;

                    uassert (15854, "CSV file ends while inside quoted field", line[0] != '\0');
                    numBytesRead += strlen( line );
                } else {
                    break;
                }
            }
            // now 'row' is string corresponding to one row of the CSV file
            // (which may span multiple lines) and represents one BSONObj
            csvTokenizeRow(row, tokens);
        }
        else {  // _type == TSV
            while (line[0] != '\t' && isspace(line[0])) { // Strip leading whitespace, but not tabs
                line++;
            }

            boost::split(tokens, line, boost::is_any_of(_sep));
        }

        // Now that the row is tokenized, create a BSONObj out of it.
        BSONObjBuilder b;
        unsigned int pos=0;
        for (vector<string>::iterator it = tokens.begin(); it != tokens.end(); ++it) {
            string token = *it;
            if ( _headerLine ) {
                _fields.push_back(token);
            }
            else {
                string name;
                if ( pos < _fields.size() ) {
                    name = _fields[pos];
                }
                else {
                    stringstream ss;
                    ss << "field" << pos;
                    name = ss.str();
                }
                pos++;

                _append( b , name , token );
            }
        }
        o = b.obj();
        return true;
    }

public:
    Import() : Tool( "import" ) {
        addFieldOptions();
        add_options()
        ("ignoreBlanks","if given, empty fields in csv and tsv will be ignored")
        ("type",po::value<string>() , "type of file to import.  default: json (json,csv,tsv)")
        ("file",po::value<string>() , "file to import from; if not specified stdin is used" )
        ("drop", "drop collection first " )
        ("headerline","CSV,TSV only - use first line as headers")
        ("upsert", "insert or update objects that already exist" )
        ("upsertFields", po::value<string>(), "comma-separated fields for the query part of the upsert. You should make sure this is indexed" )
        ("stopOnError", "stop importing at first error rather than continuing" )
        ("jsonArray", "load a json array, not one item per line. Currently limited to 16MB." )
        ;
        add_hidden_options()
        ("noimport", "don't actually import. useful for benchmarking parser" )
        ;
        addPositionArg( "file" , 1 );
        _type = JSON;
        _ignoreBlanks = false;
        _headerLine = false;
        _upsert = false;
        _doimport = true;
        _jsonArray = false;
    }

    virtual void printExtraHelp( ostream & out ) {
        out << "Import CSV, TSV or JSON data into MongoDB.\n" << endl;
    }

    int run() {
        string filename = getParam( "file" );
        long long fileSize = 0;
        int headerRows = 0;

        istream * in = &cin;

        ifstream file( filename.c_str() , ios_base::in);

        if ( filename.size() > 0 && filename != "-" ) {
            if ( ! boost::filesystem::exists( filename ) ) {
                error() << "file doesn't exist: " << filename << endl;
                return -1;
            }
            in = &file;
            fileSize = boost::filesystem::file_size( filename );
        }

        // check if we're actually talking to a machine that can write
        if (!isMaster()) {
            return -1;
        }

        string ns;

        try {
            ns = getNS();
        }
        catch (...) {
            printHelp(cerr);
            return -1;
        }

        log(1) << "ns: " << ns << endl;

        auth();

        if ( hasParam( "drop" ) ) {
            log() << "dropping: " << ns << endl;
            conn().dropCollection( ns.c_str() );
        }

        if ( hasParam( "ignoreBlanks" ) ) {
            _ignoreBlanks = true;
        }

        if ( hasParam( "upsert" ) || hasParam( "upsertFields" )) {
            _upsert = true;

            string uf = getParam("upsertFields");
            if (uf.empty()) {
                _upsertFields.push_back("_id");
            }
            else {
                StringSplitter(uf.c_str(), ",").split(_upsertFields);
            }
        }

        if ( hasParam( "noimport" ) ) {
            _doimport = false;
        }

        if ( hasParam( "type" ) ) {
            string type = getParam( "type" );
            if ( type == "json" )
                _type = JSON;
            else if ( type == "csv" ) {
                _type = CSV;
                _sep = ",";
            }
            else if ( type == "tsv" ) {
                _type = TSV;
                _sep = "\t";
            }
            else {
                error() << "don't know what type [" << type << "] is" << endl;
                return -1;
            }
        }

        if ( _type == CSV || _type == TSV ) {
            _headerLine = hasParam( "headerline" );
            if ( _headerLine ) {
                headerRows = 1;
            }
            else {
                needFields();
            }
        }

        if (_type == JSON && hasParam("jsonArray")) {
            _jsonArray = true;
        }

        time_t start = time(0);
        log(1) << "filesize: " << fileSize << endl;
        ProgressMeter pm( fileSize );
        int num = 0;
        int errors = 0;
        int len = 0;
        // buffer and line are only used when parsing a jsonArray
        boost::scoped_array<char> buffer(new char[BUF_SIZE+2]);
        char* line = buffer.get();

        while ( _jsonArray || in->rdstate() == 0 ) {
            try {
                BSONObj o;
                if (_jsonArray) {
                    int bytesProcessed = 0;
                    if (line == buffer.get()) { // Only read on first pass - the whole array must be on one line.
                        bytesProcessed = getLine(in, line);
                        line += bytesProcessed;
                        len += bytesProcessed;
                    }
                    if ((bytesProcessed = parseJSONArray(line, o)) < 0) {
                        len += bytesProcessed;
                        break;
                    }
                    len += bytesProcessed;
                    line += bytesProcessed;
                }
                else {
                    if (!parseRow(in, o, len)) {
                        continue;
                    }
                }

                if ( _headerLine ) {
                    _headerLine = false;
                }
                else if (_doimport) {
                    bool doUpsert = _upsert;
                    BSONObjBuilder b;
                    if (_upsert) {
                        for (vector<string>::const_iterator it=_upsertFields.begin(), end=_upsertFields.end(); it!=end; ++it) {
                            BSONElement e = o.getFieldDotted(it->c_str());
                            if (e.eoo()) {
                                doUpsert = false;
                                break;
                            }
                            b.appendAs(e, *it);
                        }
                    }

                    if (doUpsert) {
                        conn().update(ns, Query(b.obj()), o, true);
                    }
                    else {
                        conn().insert( ns.c_str() , o );
                    }
                }

                num++;
            }
            catch ( std::exception& e ) {
                log() << "exception:" << e.what() << endl;
                log() << line << endl;
                errors++;

                if (hasParam("stopOnError") || _jsonArray)
                    break;
            }

            if ( pm.hit( len + 1 ) ) {
                log() << "\t\t\t" << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;
            }
        }

        log() << "imported " << ( num - headerRows ) << " objects" << endl;

        conn().getLastError();

        if ( errors == 0 )
            return 0;

        error() << "encountered " << errors << " error" << ( errors == 1 ? "" : "s" ) << endl;
        return -1;
    }
};

int main( int argc , char ** argv ) {
    Import import;
    return import.main( argc , argv );
}
