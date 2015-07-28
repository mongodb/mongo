// mongo/shell/shell_utils_extended.cpp
/*
 *    Copyright 2010 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <boost/filesystem/convenience.hpp>
#include <fstream>

#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

namespace mongo {

using std::ifstream;
using std::string;
using std::stringstream;

/**
 * These utilities are thread safe but do not provide mutually exclusive access to resources
 * identified by the caller.  Dependent filesystem paths should not be accessed by different
 * threads.
 */
namespace shell_utils {

BSONObj listFiles(const BSONObj& _args, void* data) {
    BSONObj cd = BSON("0"
                      << ".");
    BSONObj args = _args.isEmpty() ? cd : _args;

    uassert(10257, "need to specify 1 argument to listFiles", args.nFields() == 1);

    BSONArrayBuilder lst;

    string rootname = args.firstElement().valuestrsafe();
    boost::filesystem::path root(rootname);
    stringstream ss;
    ss << "listFiles: no such directory: " << rootname;
    string msg = ss.str();
    uassert(12581, msg.c_str(), boost::filesystem::exists(root));

    boost::filesystem::directory_iterator end;
    boost::filesystem::directory_iterator i(root);

    while (i != end) {
        boost::filesystem::path p = *i;
        BSONObjBuilder b;
        b << "name" << p.generic_string();
        b << "baseName" << p.filename().generic_string();
        b.appendBool("isDirectory", is_directory(p));
        if (!boost::filesystem::is_directory(p)) {
            try {
                b.append("size", (double)boost::filesystem::file_size(p));
            } catch (...) {
                i++;
                continue;
            }
        }

        lst.append(b.obj());
        i++;
    }

    BSONObjBuilder ret;
    ret.appendArray("", lst.done());
    return ret.obj();
}

BSONObj ls(const BSONObj& args, void* data) {
    BSONArrayBuilder ret;
    BSONObj o = listFiles(args, data);
    if (!o.isEmpty()) {
        for (BSONObj::iterator i = o.firstElement().Obj().begin(); i.more();) {
            BSONObj f = i.next().Obj();
            string name = f["name"].String();
            if (f["isDirectory"].trueValue()) {
                name += '/';
            }
            ret << name;
        }
    }
    return BSON("" << ret.arr());
}

/** Set process wide current working directory. */
BSONObj cd(const BSONObj& args, void* data) {
    uassert(16830, "cd requires one argument -- cd(directory)", args.nFields() == 1);
    uassert(16831,
            "cd requires a string argument -- cd(directory)",
            args.firstElement().type() == String);
#if defined(_WIN32)
    std::wstring dir = toWideString(args.firstElement().String().c_str());
    if (SetCurrentDirectoryW(dir.c_str())) {
        return BSONObj();
    }
#else
    std::string dir = args.firstElement().String();
    if (chdir(dir.c_str()) == 0) {
        return BSONObj();
    }
#endif
    uasserted(16832, mongoutils::str::stream() << "cd command failed: " << errnoWithDescription());
    return BSONObj();
}

BSONObj pwd(const BSONObj&, void* data) {
    boost::filesystem::path p = boost::filesystem::current_path();
    return BSON("" << p.string());
}

BSONObj hostname(const BSONObj&, void* data) {
    return BSON("" << getHostName());
}

const int CANT_OPEN_FILE = 13300;

BSONObj cat(const BSONObj& args, void* data) {
    BSONElement e = singleArg(args);
    stringstream ss;
    ifstream f(e.valuestrsafe());
    uassert(CANT_OPEN_FILE, "couldn't open file", f.is_open());

    std::streamsize sz = 0;
    while (1) {
        char ch = 0;
        // slow...maybe change one day
        f.get(ch);
        if (ch == 0)
            break;
        ss << ch;
        sz += 1;
        uassert(13301, "cat() : file to big to load as a variable", sz < 1024 * 1024 * 16);
    }
    return BSON("" << ss.str());
}

BSONObj md5sumFile(const BSONObj& args, void* data) {
    BSONElement e = singleArg(args);
    stringstream ss;
    FILE* f = fopen(e.valuestrsafe(), "rb");
    uassert(CANT_OPEN_FILE, "couldn't open file", f);
    ON_BLOCK_EXIT(fclose, f);

    md5digest d;
    md5_state_t st;
    md5_init(&st);

    enum { BUFLEN = 4 * 1024 };
    char buffer[BUFLEN];
    int bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFLEN, f))) {
        md5_append(&st, (const md5_byte_t*)(buffer), bytes_read);
    }

    md5_finish(&st, d);
    return BSON("" << digestToString(d));
}

BSONObj mkdir(const BSONObj& args, void* data) {
    uassert(16833, "mkdir requires one argument -- mkdir(directory)", args.nFields() == 1);
    uassert(16834,
            "mkdir requires a string argument -- mkdir(directory)",
            args.firstElement().type() == String);
    boost::filesystem::create_directories(args.firstElement().String());
    return BSON("" << true);
}

BSONObj removeFile(const BSONObj& args, void* data) {
    BSONElement e = singleArg(args);
    bool found = false;

    boost::filesystem::path root(e.valuestrsafe());
    if (boost::filesystem::exists(root)) {
        found = true;
        boost::filesystem::remove_all(root);
    }

    BSONObjBuilder b;
    b.appendBool("removed", found);
    return b.obj();
}

/**
 * @param args - [ source, destination ]
 * copies file 'source' to 'destination'. Errors if the 'destination' file already exists.
 */
BSONObj copyFile(const BSONObj& args, void* data) {
    uassert(13619, "copyFile takes 2 arguments", args.nFields() == 2);

    BSONObjIterator it(args);
    const std::string source = it.next().str();
    const std::string destination = it.next().str();

    boost::filesystem::copy_file(source, destination);

    return undefinedReturn;
}

BSONObj getHostName(const BSONObj& a, void* data) {
    uassert(13411, "getHostName accepts no arguments", a.nFields() == 0);
    char buf[260];  // HOST_NAME_MAX is usually 255
    verify(gethostname(buf, 260) == 0);
    buf[259] = '\0';
    return BSON("" << buf);
}

void installShellUtilsExtended(Scope& scope) {
    scope.injectNative("getHostName", getHostName);
    scope.injectNative("removeFile", removeFile);
    scope.injectNative("copyFile", copyFile);
    scope.injectNative("listFiles", listFiles);
    scope.injectNative("ls", ls);
    scope.injectNative("pwd", pwd);
    scope.injectNative("cd", cd);
    scope.injectNative("cat", cat);
    scope.injectNative("hostname", hostname);
    scope.injectNative("md5sumFile", md5sumFile);
    scope.injectNative("mkdir", mkdir);
}
}
}
