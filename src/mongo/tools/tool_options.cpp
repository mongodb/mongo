/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/tools/tool_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/password.h"

namespace mongo {

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addGeneralToolOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("help", "help", moe::Switch,
                    "produce help message", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("verbose", "verbose,v", moe::Switch,
                    "be more verbose (include multiple times "
                    "for more verbosity e.g. -vvvvv)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("quiet", "quiet", moe::Switch,
                    "silence all non error diagnostic messages", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("version", "version", moe::Switch,
                    "print the program's version and exit", true));
        if(!ret.isOK()) {
            return ret;
        }

        /* support for -vv -vvvv etc. */
        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            ret = options->addOption(OD(s.c_str(), s.c_str(), moe::Switch, "verbose", false));
            if(!ret.isOK()) {
                return ret;
            }
        }

        return Status::OK();
    }

    Status addRemoteServerToolOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("host", "host,h", moe::String,
                    "mongo host to connect to ( <set name>/s1,s2 for sets)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("port", "port", moe::String,
                    "server port. Can also use --host hostname:port", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("ipv6", "ipv6", moe::Switch,
                    "enable IPv6 support (disabled by default)", true));
        if(!ret.isOK()) {
            return ret;
        }
#ifdef MONGO_SSL
        ret = options->addOption(OD("ssl", "ssl", moe::Switch,
                    "use SSL for all connections", true));
        if(!ret.isOK()) {
            return ret;
        }
#endif

        ret = options->addOption(OD("username", "username,u", moe::String, "username", true));
        if(!ret.isOK()) {
            return ret;
        }
        // We ask a user for a password if they pass in an empty string or pass --password with no
        // argument.  This must be handled when the password value is checked.
        //
        // Desired behavior:
        // --username test --password test // Continue with username "test" and password "test"
        // --username test // Continue with username "test" and no password
        // --username test --password // Continue with username "test" and prompt for password
        // --username test --password "" // Continue with username "test" and prompt for password
        //
        // To do this we pass moe::Value(std::string("")) as the "implicit value" of this option
        ret = options->addOption(OD("password", "password,p", moe::String,
                    "password", true, moe::Value(), moe::Value(std::string(""))));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("authenticationDatabase", "authenticationDatabase", moe::String,
                    "user source (defaults to dbname)", true,
                    moe::Value(std::string(""))));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("authenticationMechanism", "authenticationMechanism",
                    moe::String,
                    "authentication mechanism", true,
                    moe::Value(std::string("MONGODB-CR"))));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addLocalServerToolOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("dbpath", "dbpath",moe::String,
                    "directly access mongod database files in the given path, instead of "
                    "connecting to a mongod  server - needs to lock the data directory, "
                    "so cannot be used if a mongod is currently accessing the same path",
                    true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("directoryperdb", "directoryperdb", moe::Switch,
                    "each db is in a separate directly "
                    "(relevant only if dbpath specified)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("journal", "journal", moe::Switch,
                    "enable journaling (relevant only if dbpath specified)", true));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }
    Status addSpecifyDBCollectionToolOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("db", "db,d",moe::String, "database to use", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("collection", "collection,c",moe::String,
                    "collection to use (some commands)", true));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addFieldOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("fields", "fields,f", moe::String ,
                    "comma separated list of field names e.g. -f name,age", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("fieldFile", "fieldFile", moe::String ,
                    "file with field names - 1 per line", true));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addBSONToolOptions(moe::OptionSection* options) {
        Status ret = options->addOption(OD("objcheck", "objcheck", moe::Switch,
                    "validate object before inserting (default)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("noobjcheck", "noobjcheck", moe::Switch,
                    "don't validate object before inserting", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("filter", "filter", moe::String ,
                    "filter to apply before inserting", true));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("out", "out,o", moe::String,
                    "output directory or \"-\" for stdout", true,
                    moe::Value(std::string("dump"))));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("query", "query,q", moe::String , "json query", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplog", "oplog", moe::Switch,
                    "Use oplog for point-in-time snapshotting", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("repair", "repair", moe::Switch,
                    "try to recover a crashed database", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("forceTableScan", "forceTableScan", moe::Switch,
                    "force a table scan (do not use $snapshot)"));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoRestoreOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("drop", "drop", moe::Switch,
                    "drop each collection before import", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplogReplay", "oplogReplay", moe::Switch,
                    "replay oplog for point-in-time restore", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplogLimit", "oplogLimit", moe::String,
                    "include oplog entries before the provided Timestamp "
                    "(seconds[:ordinal]) during the oplog replay; "
                    "the ordinal value is optional", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("keepIndexVersion", "keepIndexVersion", moe::Switch,
                    "don't upgrade indexes to newest version", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("noOptionsRestore", "noOptionsRestore", moe::Switch,
                    "don't restore collection options", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("noIndexRestore", "noIndexRestore", moe::Switch,
                    "don't restore indexes", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("w", "w", moe::Int ,
                    "minimum number of replicas per write" , true, moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }

        // left in for backwards compatibility
        ret = options->addOption(OD("indexesLast", "indexesLast", moe::Switch,
                    "wait to add indexes (now default)", false));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "dir", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoExportOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addFieldOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("query", "query,q", moe::String ,
                    "query filter, as a JSON string", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("csv", "csv", moe::Switch,
                    "export to csv instead of json", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("out", "out,o", moe::String,
                    "output file; if not specified, stdout is used", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("jsonArray", "jsonArray", moe::Switch,
                    "output to a json array rather than one object per line", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("slaveOk", "slaveOk,k", moe::Bool ,
                    "use secondaries for export if available, default true", true,
                    moe::Value(true)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("forceTableScan", "forceTableScan", moe::Switch,
                    "force a table scan (do not use $snapshot)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("skip", "skip", moe::Int,
                    "documents to skip, default 0", true, moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("limit", "limit", moe::Int,
                    "limit the numbers of documents returned, default all", true,
                    moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoImportOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addFieldOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("ignoreBlanks", "ignoreBlanks", moe::Switch,
                    "if given, empty fields in csv and tsv will be ignored", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("type", "type",moe::String,
                    "type of file to import.  default: json (json,csv,tsv)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("file", "file",moe::String,
                    "file to import from; if not specified stdin is used", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("drop", "drop", moe::Switch, "drop collection first ", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("headerline", "headerline", moe::Switch,
                    "first line in input file is a header (CSV and TSV only)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("upsert", "upsert", moe::Switch,
                    "insert or update objects that already exist", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("upsertFields", "upsertFields", moe::String,
                    "comma-separated fields for the query part of the upsert. "
                    "You should make sure this is indexed", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("stopOnError", "stopOnError", moe::Switch,
                    "stop importing at first error rather than continuing", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("jsonArray", "jsonArray", moe::Switch,
                    "load a json array, not one item per line. "
                    "Currently limited to 16MB.", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("noimport", "noimport", moe::Switch,
                    "don't actually import. useful for benchmarking parser", false));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "file", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoFilesOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("local", "local,l", moe::String,
                    "local filename for put|get (default is to use the same name as "
                    "'gridfs filename')", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("type", "type,t", moe::String,
                    "MIME type for put (default is to omit)", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("replace", "replace,r", moe::Switch,
                    "Remove other files with same name after PUT", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "command", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addPositionalOption(POD( "file", moe::String, 2 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoOplogOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("seconds", "seconds,s", moe::Int ,
                    "seconds to go back default:86400", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("from", "from", moe::String , "host to pull from", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplogns", "oplogns", moe::String ,
                    "ns to pull from" , true, moe::Value(std::string("local.oplog.rs"))));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoStatOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("noheaders", "noheaders", moe::Switch,
                    "don't output column names", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("rowcount", "rowcount,n", moe::Int,
                    "number of stats lines to print (0 for indefinite)", true,
                    moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("http", "http", moe::Switch,
                    "use http instead of raw db connection", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("discover", "discover", moe::Switch,
                    "discover nodes and display stats for all", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("all", "all", moe::Switch,
                    "all optional fields", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "sleep", moe::Int, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addMongoTopOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("locks", "locks", moe::Switch,
                    "use db lock info instead of top", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "sleep", moe::Int, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    Status addBSONDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("type", "type", moe::String ,
                    "type of output: json,debug", true, moe::Value(std::string("json"))));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "file", moe::String, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoDumpHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Export MongoDB data to BSON files.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoRestoreHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Import BSON files into MongoDB.\n" << std::endl;
        *out << "usage: mongorestore [options] [directory or filename to restore from]"
             << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoExportHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Export MongoDB data to CSV, TSV or JSON files.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoImportHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Import CSV, TSV or JSON data into MongoDB.\n" << std::endl;
        *out << "When importing JSON documents, each document must be a separate line of the input file.\n";
        *out << "\nExample:\n";
        *out << "  mongoimport --host myhost --db my_cms --collection docs < mydocfile.json\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoFilesHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Browse and modify a GridFS filesystem.\n" << std::endl;
        *out << "usage: mongofiles [options] command [gridfs filename]" << std::endl;
        *out << "command:" << std::endl;
        *out << "  one of (list|search|put|get)" << std::endl;
        *out << "  list - list all files.  'gridfs filename' is an optional prefix " << std::endl;
        *out << "         which listed filenames must begin with." << std::endl;
        *out << "  search - search all files. 'gridfs filename' is a substring " << std::endl;
        *out << "           which listed filenames must contain." << std::endl;
        *out << "  put - add a file with filename 'gridfs filename'" << std::endl;
        *out << "  get - get a file with filename 'gridfs filename'" << std::endl;
        *out << "  delete - delete all files with filename 'gridfs filename'" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoOplogHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Pull and replay a remote MongoDB oplog.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printMongoStatHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "View live MongoDB performance statistics.\n" << std::endl;
        *out << "usage: mongostat [options] [sleep time]" << std::endl;
        *out << "sleep time: time to wait (in seconds) between calls" << std::endl;
        *out << options.helpString();
        *out << "\n";
        *out << " Fields\n";
        *out << "   inserts  \t- # of inserts per second (* means replicated op)\n";
        *out << "   query    \t- # of queries per second\n";
        *out << "   update   \t- # of updates per second\n";
        *out << "   delete   \t- # of deletes per second\n";
        *out << "   getmore  \t- # of get mores (cursor batch) per second\n";
        *out << "   command  \t- # of commands per second, on a slave its local|replicated\n";
        *out << "   flushes  \t- # of fsync flushes per second\n";
        *out << "   mapped   \t- amount of data mmaped (total data size) megabytes\n";
        *out << "   vsize    \t- virtual size of process in megabytes\n";
        *out << "   res      \t- resident size of process in megabytes\n";
        *out << "   faults   \t- # of pages faults per sec\n";
        *out << "   locked   \t- name of and percent time for most locked database\n";
        *out << "   idx miss \t- percent of btree page misses (sampled)\n";
        *out << "   qr|qw    \t- queue lengths for clients waiting (read|write)\n";
        *out << "   ar|aw    \t- active clients (read|write)\n";
        *out << "   netIn    \t- network traffic in - bytes\n";
        *out << "   netOut   \t- network traffic out - bytes\n";
        *out << "   conn     \t- number of open connections\n";
        *out << "   set      \t- replica set name\n";
        *out << "   repl     \t- replication type \n";
        *out << "            \t    PRI - primary (master)\n";
        *out << "            \t    SEC - secondary\n";
        *out << "            \t    REC - recovering\n";
        *out << "            \t    UNK - unknown\n";
        *out << "            \t    SLV - slave\n";
        *out << "            \t    RTR - mongos process (\"router\")\n";
        *out << std::flush;
    }

    void printMongoTopHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "View live MongoDB collection statistics.\n" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }

    void printBSONDumpHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "Display BSON objects in a data file.\n" << std::endl;
        *out << "usage: bsondump [options] <bson filename>" << std::endl;
        *out << options.helpString();
        *out << std::flush;
    }
}
