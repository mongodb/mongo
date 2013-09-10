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

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>
#include <fstream>
#include <iostream>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/json.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/text.h"

using namespace mongo;
using std::string;
using std::stringstream;

namespace mongo {
    MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {

        options = moe::OptionSection( "options" );
        moe::OptionsParser parser;

        Status retStatus = addMongoImportOptions(&options);
        if (!retStatus.isOK()) {
            return retStatus;
        }

        retStatus = parser.run(options, context->args(), context->env(), &_params);
        if (!retStatus.isOK()) {
            std::ostringstream oss;
            oss << retStatus.toString() << "\n";
            printMongoImportHelp(options, &oss);
            return Status(ErrorCodes::FailedToParse, oss.str());
        }

        return Status::OK();
    }
} // namespace mongo

class Import : public Tool {

    enum Type { JSON , CSV , TSV };
    Type _type;

    const char * _sep;
    bool _ignoreBlanks;
    bool _headerLine;
    bool _upsert;
    bool _doimport;
    vector<string> _upsertFields;
    static const int BUF_SIZE;

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
        in->getline( buf , BUF_SIZE );
        if ((in->rdstate() & ios_base::eofbit) && (in->rdstate() & ios_base::failbit)) {
            // this is the last line, and it's empty (not even a newline)
            buf[0] = '\0';
            return 0;
        }

        uassert(16329, str::stream() << "read error, or input line too long (max length: "
                << BUF_SIZE << ")", !(in->rdstate() & ios_base::failbit));
        LOG(1) << "got line:" << buf << endl;

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
     * description:
     *     given a pointer into a JSON array, returns the next valid JSON object
     * args:
     *     buf - pointer into the JSON array
     *     o - BSONObj to fill
     *     numBytesRead - return parameter for how far we read
     * return:
     *     true if an object was successfully parsed
     *     false if there is no object left to parse
     * throws:
     *     exception on parsing error
     */
    bool parseJSONArray(char* buf, BSONObj* o, int* numBytesRead) {

        // Skip extra characters since fromjson must be passed a character buffer that starts with a
        // valid JSON object, and does not accept JSON arrays.
        // (NOTE: this doesn't catch all invalid JSON arrays, but does fail on invalid characters)
        *numBytesRead = 0;
        while (buf[0] == '[' ||
               buf[0] == ']' ||
               buf[0] == ',' ||
               isspace(buf[0])) {
            (*numBytesRead)++;
            buf++;
        }

        if (buf[0] == '\0')
            return false;

        try {
            int len = 0;
            *o = fromjson(buf, &len);
            (*numBytesRead) += len;
        } catch ( MsgAssertionException& e ) {
            uasserted(13293, string("Invalid JSON passed to mongoimport: ") + e.what());
        }

        return true;
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

                    uassert(15854, "CSV file ends while inside quoted field", line[0] != '\0');
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
        _type = JSON;
        _ignoreBlanks = false;
        _headerLine = false;
        _upsert = false;
        _doimport = true;
    }

    virtual void printHelp( ostream & out ) {
        printMongoImportHelp(options, &out);
    }

    unsigned long long lastErrorFailures;

    /** @return true if ok */
    bool checkLastError() { 
        string s = conn().getLastError();
        if( !s.empty() ) { 
            if( str::contains(s,"uplicate") ) {
                // we don't want to return an error from the mongoimport process for
                // dup key errors
                log() << s << endl;
            }
            else {
                lastErrorFailures++;
                log() << "error: " << s << endl;
                return false;
            }
        }
        return true;
    }

    void importDocument (const std::string &ns, const BSONObj& o) {
        bool doUpsert = _upsert;
        BSONObjBuilder b;
        if (_upsert) {
            for (vector<string>::const_iterator it = _upsertFields.begin(),
                 end = _upsertFields.end(); it != end; ++it) {
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
            conn().insert(ns.c_str(), o);
        }
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
        catch (int e) {
            if (e == -1) {
                // no collection specified - use name of collection that was dumped from
                string oldCollName = boost::filesystem::path(filename).leaf().string();
                oldCollName = oldCollName.substr( 0 , oldCollName.find_last_of( "." ) );
                cerr << "using filename '" << oldCollName << "' as collection." << endl;
                ns = _db + "." + oldCollName;
            }
            else {
                printHelp(cerr);
                return -1;
            }
        }
        catch (...) {
            printHelp(cerr);
            return -1;
        }

        LOG(1) << "ns: " << ns << endl;

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


        time_t start = time(0);
        LOG(1) << "filesize: " << fileSize << endl;
        ProgressMeter pm( fileSize );
        int num = 0;
        int lastNumChecked = num;
        int errors = 0;
        lastErrorFailures = 0;
        int len = 0;

        // We have to handle jsonArrays differently since we can't read line by line
        if (_type == JSON && hasParam("jsonArray")) {

            // We cycle through these buffers in order to continuously read from the stream
            boost::scoped_array<char> buffer1(new char[BUF_SIZE]);
            boost::scoped_array<char> buffer2(new char[BUF_SIZE]);
            char* current_buffer = buffer1.get();
            char* next_buffer = buffer2.get();
            char* temp_buffer;

            // buffer_base_offset is the offset into the stream where our buffer starts, while
            // input_stream_offset is the total number of bytes read from the stream
            uint64_t buffer_base_offset = 0;
            uint64_t input_stream_offset = 0;

            // Fill our buffer
            // NOTE: istream::get automatically appends '\0' at the end of what it reads
            in->get(current_buffer, BUF_SIZE, '\0');
            uassert(16808, str::stream() << "read error: " << strerror(errno), !in->fail());

            // Record how far we read into the stream.
            input_stream_offset += in->gcount();

            while (true) {
                try {

                    BSONObj o;

                    // Try to parse (parseJSONArray)
                    if (!parseJSONArray(current_buffer, &o, &len)) {
                        break;
                    }

                    // Import documents
                    if (_doimport) {
                        importDocument(ns, o);

                        if (num < 10) {
                            // we absolutely want to check the first and last op of the batch. we do
                            // a few more as that won't be too time expensive.
                            checkLastError();
                            lastNumChecked = num;
                        }
                    }

                    // Copy over the part of buffer that was not parsed
                    strcpy(next_buffer, current_buffer + len);

                    // Advance our buffer base past what we've already parsed
                    buffer_base_offset += len;

                    // Fill up the end of our next buffer only if there is something in the stream
                    if (!in->eof()) {
                        // NOTE: istream::get automatically appends '\0' at the end of what it reads
                        in->get(next_buffer + (input_stream_offset - buffer_base_offset),
                                BUF_SIZE - (input_stream_offset - buffer_base_offset), '\0');
                        uassert(16809, str::stream() << "read error: "
                                                     << strerror(errno), !in->fail());

                        // Record how far we read into the stream.
                        input_stream_offset += in->gcount();
                    }

                    // Swap buffer pointers
                    temp_buffer = current_buffer;
                    current_buffer = next_buffer;
                    next_buffer = temp_buffer;

                    num++;
                }
                catch ( const std::exception& e ) {
                    log() << "exception: " << e.what()
                          << ", current buffer: " << current_buffer << endl;
                    errors++;

                    // Since we only support JSON arrays all on one line, we might as well stop now
                    // because we can't read any more documents
                    break;
                }

                if ( pm.hit( len + 1 ) ) {
                    log() << "\t\t\t" << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;
                }
            }
        }
        else {
            while (in->rdstate() == 0) {
                try {
                    BSONObj o;

                    if (!parseRow(in, o, len)) {
                        continue;
                    }

                    if ( _headerLine ) {
                        _headerLine = false;
                    }
                    else if (_doimport) {
                        importDocument(ns, o);

                        if (num < 10) {
                            // we absolutely want to check the first and last op of the batch. we do
                            // a few more as that won't be too time expensive.
                            checkLastError();
                            lastNumChecked = num;
                        }
                    }

                    num++;
                }
                catch ( const std::exception& e ) {
                    log() << "exception:" << e.what() << endl;
                    errors++;

                    if (hasParam("stopOnError"))
                        break;
                }

                if ( pm.hit( len + 1 ) ) {
                    log() << "\t\t\t" << num << "\t" << ( num / ( time(0) - start ) ) << "/second" << endl;
                }
            }
        }

        // this is for two reasons: to wait for all operations to reach the server and be processed, and this will wait until all data reaches the server,
        // and secondly to check if there were an error (on the last op)
        if( lastNumChecked+1 != num ) { // avoid redundant log message if already reported above
            log() << "check " << lastNumChecked << " " << num << endl;
            checkLastError();
        }

        bool hadErrors = lastErrorFailures || errors;

        // the message is vague on lastErrorFailures as we don't call it on every single operation. 
        // so if we have a lastErrorFailure there might be more than just what has been counted.
        log() << (lastErrorFailures ? "tried to import " : "imported ") << ( num - headerRows ) << " objects" << endl;

        if ( !hadErrors )
            return 0;

        error() << "encountered " << (lastErrorFailures?"at least ":"") << lastErrorFailures+errors <<  " error(s)" << ( lastErrorFailures+errors == 1 ? "" : "s" ) << endl;
        return -1;
    }
};

const int Import::BUF_SIZE(1024 * 1024 * 16);

REGISTER_MONGO_TOOL(Import);
