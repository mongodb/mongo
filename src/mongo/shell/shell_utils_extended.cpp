/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>  // IWYU pragma: keep
#include <string>
#include <system_error>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_bin_util.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/util/bson_corpus.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/scripting/engine.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/icu.h"
#include "mongo/util/md5.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/password.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


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
namespace {

BSONObj listFiles(const BSONObj& _args, void* data) {
    BSONObj cd = BSON("0" << ".");
    BSONObj args = _args.isEmpty() ? cd : _args;

    uassert(10257, "need to specify 1 argument to listFiles", args.nFields() == 1);

    BSONArrayBuilder lst;

    string rootname = args.firstElement().str();
    boost::filesystem::path root(rootname);
    stringstream ss;
    ss << "listFiles: no such directory: " << rootname;
    string msg = ss.str();
    uassert(12581,
            msg.c_str(),
            boost::filesystem::exists(root) && boost::filesystem::is_directory(root));


    for (boost::filesystem::directory_iterator i(root), end; i != end; ++i)
        try {
            const boost::filesystem::path& p = *i;
            BSONObjBuilder b;
            b << "name" << p.generic_string();
            b << "baseName" << p.filename().generic_string();
            const bool isDirectory = is_directory(p);
            b.appendBool("isDirectory", isDirectory);
            if (boost::filesystem::exists(p) && boost::filesystem::is_regular_file(p)) {
                b.append("size", (double)boost::filesystem::file_size(p));
            }

            lst.append(b.obj());
        } catch (const boost::filesystem::filesystem_error&) {
            continue;  // Filesystem errors cause us to just skip that entry, entirely.
        }

    BSONObjBuilder ret;
    ret.appendArray("", lst.done());
    return ret.obj();
}

/** Set process wide current working directory. */
BSONObj cd(const BSONObj& args, void* data) {
    uassert(16830, "cd requires one argument -- cd(directory)", args.nFields() == 1);
    uassert(16831,
            "cd requires a string argument -- cd(directory)",
            args.firstElement().type() == BSONType::string);
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
    auto ec = lastSystemError();
    uasserted(16832, str::stream() << "cd command failed: " << errorMessage(ec));
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
    BSONObjIterator it(args);

    auto filePath = it.next();
    uassert(51012,
            "the first argument to cat() must be a string containing the path to the file",
            filePath.type() == BSONType::string);

    std::ios::openmode mode = std::ios::in;

    auto useBinary = it.next();
    if (!useBinary.eoo()) {
        uassert(51013,
                "the second argument to cat(), must be a boolean indicating whether "
                "or not to read the file in binary mode. If omitted, the default is 'false'.",
                useBinary.type() == BSONType::boolean);

        if (useBinary.Bool())
            mode |= std::ios::binary;
    }

    ifstream f(filePath.valueStringDataSafe().data(), mode);
    uassert(CANT_OPEN_FILE, fmt::format("couldn't open file {}", filePath.str()), f.is_open());
    std::streamsize fileSize = 0;
    // will throw on filesystem error
    fileSize = boost::filesystem::file_size(filePath.str());
    static constexpr auto kFileSizeLimit = 1024 * 1024 * 16;
    uassert(
        13301,
        fmt::format(
            "cat() : file {} too big to load as a variable (file is {} bytes, limit is {} bytes.)",
            filePath.str(),
            fileSize,
            kFileSizeLimit),
        fileSize < kFileSizeLimit);

    std::ostringstream ss;
    ss << f.rdbuf();

    return BSON("" << ss.str());
}

BSONObj copyFileRange(const BSONObj& args, void* data) {
    uassert(4793600,
            "copyFileRange() requires 4 arguments: copyFileRange(src, dest, offset, length)",
            args.nFields() == 4);

    BSONObjIterator it(args);

    const std::string src = it.next().str();
    const std::string dest = it.next().str();
    int64_t offset = it.next().Long();
    int64_t length = it.next().Long();

    std::ifstream in(src, std::ios::binary | std::ios::in);
    uassert(CANT_OPEN_FILE, fmt::format("Couldn't open file {} for reading", src), in.is_open());

    in.exceptions(std::ifstream::badbit);

    // Set the position using the given offset.
    in.seekg(offset, std::ios::beg);
    if (in.rdstate() & std::ifstream::eofbit) {
        // Offset is past EOF.
        in.close();
        return BSON("n" << 0 << "earlyEOF" << true);
    }

    bool earlyEOF = false;
    std::vector<char> buffer(length);
    if (!in.read(buffer.data(), length)) {
        invariant(in.rdstate() & std::ifstream::eofbit);
        earlyEOF = true;
    }

    int64_t bytesRead = in.gcount();
    invariant(bytesRead <= length);
    in.close();

    // Before opening 'dest', check if we need to resize the file to fit in the contents.
    if (static_cast<uint64_t>(offset + bytesRead) > boost::filesystem::file_size(dest)) {
        boost::filesystem::resize_file(dest, offset + bytesRead);
    }

    std::ofstream out(dest, std::ios::binary | std::ios::out | std::ios::in);
    uassert(CANT_OPEN_FILE, fmt::format("Couldn't open file {} for writing", dest), out.is_open());

    out.exceptions(std::ofstream::eofbit | std::ofstream::failbit | std::ofstream::badbit);

    out.seekp(offset, std::ios::beg);
    out.write(buffer.data(), bytesRead);
    out.close();

    uassert(
        9663000,
        fmt::format(
            "Couldn't write {} bytes starting at {} from file {} to {}", length, offset, src, dest),
        out);

    return BSON("n" << bytesRead << "earlyEOF" << earlyEOF);
}

BSONObj md5sumFile(const BSONObj& args, void* data) {
    BSONElement e = singleArg(args);
    stringstream ss;
    FILE* f = fopen(e.valueStringDataSafe().data(), "rb");
    uassert(CANT_OPEN_FILE, str::stream() << "couldn't open file " << e.str(), f);
    ON_BLOCK_EXIT([&] { fclose(f); });

    md5digest d;
    md5_state_t st;
    md5_init_state(&st);

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
            args.firstElement().type() == BSONType::string);

    // Boost bug 12495 (https://svn.boost.org/trac/boost/ticket/12495):
    // create_directories crashes on empty string. We expect mkdir("") to
    // fail on the OS level anyway, so catch it here instead.
    uassert(40315, "mkdir requires a non-empty string", args.firstElement().String() != "");

    boost::system::error_code ec;
    auto created = boost::filesystem::create_directories(args.firstElement().String(), ec);

    uassert(40316, "mkdir() failed: " + ec.message(), !ec);

    BSONObjBuilder wrapper;
    BSONObjBuilder res(wrapper.subobjStart(""));
    res.append("exists", true);
    res.append("created", created);
    res.done();
    return wrapper.obj();
}

/**
 * @param args - [ source, destination ]
 * copies directory 'source' to 'destination'. Errors if the 'destination' file already exists.
 */
BSONObj copyDir(const BSONObj& args, void* data) {
    uassert(8423308, "copyDir takes 2 arguments", args.nFields() == 2);

    BSONObjIterator it(args);
    const std::string source = it.next().str();
    const std::string destination = it.next().str();

    boost::filesystem::copy(source, destination, boost::filesystem::copy_options::recursive);

    return undefinedReturn;
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

    bool success = boost::filesystem::copy_file(source, destination);
    uassert(
        9663001, str::stream() << "failed to copy " << source << " to " << destination, success);

    return undefinedReturn;
}

BSONObj writeFile(const BSONObj& args, void* data) {
    // Parse the arguments.

    uassert(40340,
            "writeFile requires at least 2 arguments: writeFile(filePath, content, "
            "[useBinaryMode])",
            args.nFields() >= 2);

    BSONObjIterator it(args);

    auto filePathElem = it.next();
    uassert(40341,
            "the first argument to writeFile() must be a string containing the path to the file",
            filePathElem.type() == BSONType::string);

    auto fileContentElem = it.next();
    uassert(40342,
            "the second argument to writeFile() must be a string to write to the file",
            fileContentElem.type() == BSONType::string);

    // Limit the capability to writing only new, regular files in existing directories.

    const boost::filesystem::path originalFilePath{filePathElem.String()};
    const boost::filesystem::path normalizedFilePath{originalFilePath.lexically_normal()};
    const boost::filesystem::path absoluteFilePath{boost::filesystem::absolute(normalizedFilePath)};

    uassert(40343,
            "writeFile() can only write a file in a directory which already exists",
            boost::filesystem::exists(absoluteFilePath.parent_path()));
    uassert(40344,
            "writeFile() can only write to a file which does not yet exist",
            !boost::filesystem::exists(absoluteFilePath));
    uassert(40345,
            "the file name must be compatible with POSIX and Windows",
            boost::filesystem::portable_name(absoluteFilePath.filename().string()));

    std::ios::openmode mode = std::ios::out;

    auto useBinary = it.next();
    if (!useBinary.eoo()) {
        uassert(51014,
                "the third argument to writeFile(), must be a boolean indicating whether "
                "or not to read the file in binary mode. If omitted, the default is 'false'.",
                useBinary.type() == BSONType::boolean);

        if (useBinary.Bool())
            mode |= std::ios::binary;
    }

    boost::filesystem::ofstream ofs{absoluteFilePath, mode};
    uassert(40346,
            str::stream() << "failed to open file " << absoluteFilePath.string() << " for writing",
            ofs);

    ofs << fileContentElem.String();
    uassert(40347, str::stream() << "failed to write to file " << absoluteFilePath.string(), ofs);

    return undefinedReturn;
}

BSONObj appendFile(const BSONObj& args, void* data) {
    // Parse the arguments.

    uassert(9380001,
            "appendFile requires at least 2 arguments: appendFile(filePath, content, "
            "[useBinaryMode])",
            args.nFields() >= 2);

    BSONObjIterator it(args);

    auto filePathElem = it.next();
    uassert(9380002,
            "the first argument to appendFile() must be a string containing the path to the file",
            filePathElem.type() == BSONType::string);

    auto fileContentElem = it.next();
    uassert(9380003,
            "the second argument to appendFile() must be a string to append to the file",
            fileContentElem.type() == BSONType::string);

    // Limit the capability to appending to only existing files.

    const boost::filesystem::path originalFilePath{filePathElem.String()};
    const boost::filesystem::path normalizedFilePath{originalFilePath.lexically_normal()};
    const boost::filesystem::path absoluteFilePath{boost::filesystem::absolute(normalizedFilePath)};

    uassert(9380004,
            "appendFile() can onlyappend to a file in a directory that already exists",
            boost::filesystem::exists(absoluteFilePath.parent_path()));
    uassert(9380005,
            "appendFile() can only append to a file that does not yet exist",
            boost::filesystem::exists(absoluteFilePath));
    uassert(9380006,
            "the file name must be compatible with POSIX and Windows",
            boost::filesystem::portable_name(absoluteFilePath.filename().string()));

    std::ios::openmode mode = std::ios::out;

    auto useBinary = it.next();
    if (!useBinary.eoo()) {
        uassert(9380007,
                "the third argument to appendFile(), must be a boolean indicating whether "
                "or not to read the file in binary mode. If omitted, the default is 'false'.",
                useBinary.type() == BSONType::boolean);

        if (useBinary.Bool())
            mode |= std::ios::binary;
    }

    boost::filesystem::ofstream ofs{absoluteFilePath, mode | std::ios::app};
    uassert(9380008,
            str::stream() << "failed to open file " << absoluteFilePath.string()
                          << " for appending",
            ofs);

    ofs << fileContentElem.String();
    uassert(
        9380009, str::stream() << "failed to append to file " << absoluteFilePath.string(), ofs);

    return undefinedReturn;
}

BSONObj getHostName(const BSONObj& a, void* data) {
    uassert(13411, "getHostName accepts no arguments", a.nFields() == 0);
    char buf[260];  // HOST_NAME_MAX is usually 255
    MONGO_verify(gethostname(buf, 260) == 0);
    buf[259] = '\0';
    return BSON("" << buf);
}

BSONObj passwordPrompt(const BSONObj& a, void* data) {
    uassert(50890, "passwordPrompt accepts no arguments", a.nFields() == 0);
    return BSON("" << askPassword());
}

BSONObj changeUmask(const BSONObj& a, void* data) {
#ifdef _WIN32
    uasserted(50977, "umask is not supported on windows");
#else
    uassert(50976,
            "umask takes 1 argument, the octal mode of the umask",
            a.nFields() == 1 && isNumericBSONType(a.firstElementType()));
    auto val = a.firstElement().safeNumberInt();
    return BSON("" << static_cast<int>(umask(static_cast<mode_t>(val))));
#endif
}

BSONObj getFileMode(const BSONObj& a, void* data) {
    uassert(50975,
            "getFileMode() takes one argument, the absolute path to a file",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);
    auto pathStr = a.firstElement().checkAndGetStringData();
    boost::filesystem::path path(pathStr.data());
    boost::system::error_code ec;
    auto fileStatus = boost::filesystem::status(path, ec);
    if (ec) {
        uasserted(50974,
                  str::stream() << "Unable to get status for file \"" << pathStr
                                << "\": " << ec.message());
    }

    return BSON("" << fileStatus.permissions());
}

BSONObj decompressBSONColumn(const BSONObj& a, void* data) {
    uassert(ErrorCodes::InvalidOptions,
            "decompressBSONColumn() takes one argument, the BSONColumn BinData element",
            a.nFields() == 1 && a.firstElementType() == BSONType::binData &&
                a.firstElement().binDataType() == BinDataType::Column);

    BSONColumn column(a.firstElement());

    BSONObjBuilder wrapper;
    BSONObjBuilder res(wrapper.subobjStart(""));

    size_t index = 0;
    for (const BSONElement& e : column) {
        if (e.eoo()) {
            index++;
        } else {
            res.appendAs(e, std::to_string(index++));
        }
    }
    res.done();

    return wrapper.obj();
}

/**
 * Dumps BSON data as a Hex-formatted string
 */
BSONObj dumpBSONAsHex(const BSONObj& a, void* data) {
    uassert(9174601,
            "dumpBSONAsHex() takes one argument: a BSON obj",
            a.nFields() == 1 && a.firstElementType() == BSONType::object);
    auto obj = a.firstElement().Obj();

    return BSON("" << hexblob::encodeLower(obj.objdata(), obj.objsize()));
}

/**
 * Convert a hex string encoded BSON into its BSONObj form.
 */
BSONObj hexToBSON(const BSONObj& a, void*) {
    uassert(9174600,
            "hexToBSON takes one argument: a hex string",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);

    BufBuilder bb;
    hexblob::decode(a.firstElement().String(), &bb);
    return BSON("" << BSONObj(bb.release()));
}

BSONObj generateStorageBSON(const BSONObj& args, void* data) {
    uassert(9492300,
            "generateStorageBSON() requires 4 arguments: generateStorageBSON(dbpath, "
            "storageEngine, directoryPerDB, directoryForIndexes)",
            args.nFields() == 4);

    // Writes out a storage.bson file in the given dbpath.
    BSONObjIterator it(args);

    const std::string dbpath = it.next().str();
    const std::string storageEngine = it.next().str();
    const bool directoryPerDB = it.next().booleanSafe();
    const bool directoryForIndexes = it.next().booleanSafe();

    // Generates a document with the following structure.
    // {
    //     storage: {
    //         engine: <string>,
    //         options: {
    //             directoryPerDB: <bool>,
    //             directoryForIndexes: <bool>
    //         }
    //     }
    // }
    BSONObj storage =
        BSON("storage" << BSON("engine"
                               << storageEngine << "options"
                               << BSON("directoryPerDB" << directoryPerDB << "directoryForIndexes"
                                                        << directoryForIndexes)));

    boost::filesystem::path storageBSONPath = boost::filesystem::path(dbpath) / "storage.bson";

    std::ofstream out(storageBSONPath.string(), std::ios::binary | std::ios::out | std::ios::trunc);
    uassert(CANT_OPEN_FILE,
            fmt::format("Couldn't open file {} for writing", storageBSONPath.string()),
            out.is_open());

    out.exceptions(std::ofstream::eofbit | std::ofstream::failbit | std::ofstream::badbit);

    out.write(storage.objdata(), storage.objsize());
    out.close();

    return BSON("ok" << 1);
}

BSONObj shellGetEnv(const BSONObj& a, void*) {
    uassert(4671206,
            "_getEnv() takes one argument: the name of the environment variable",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);
    const auto envName = a.firstElement().String();
    std::string result{};
#ifndef _WIN32
    auto envPtr = getenv(envName.c_str());
    if (envPtr) {
        result = std::string(envPtr);
    }
#else
    auto envPtr = _wgetenv(toNativeString(envName.c_str()).c_str());
    if (envPtr) {
        result = toUtf8String(envPtr);
    }
#endif

    return BSON("" << result.c_str());
}

BSONObj getStringWidth(const BSONObj& a, void* data) {
    uassert(8730901,
            "getStringWidth takes a single string argument",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);
    const auto str = a.firstElement().valueStringData();
    int width = icuGetStringWidth(str, false, true);
    return BSON("" << width);
}

}  // namespace

/**
 * Writes an array of bson objects one after another. The format is readable by the `bsondump` tool.
 */
BSONObj writeBsonArrayToFile(const BSONObj& args, void* data) {
    uassert(7196709, "writeBsonArrayToFile needs 2 arguments", args.nFields() == 2);

    BSONObjIterator it(args);
    auto filePathElem = it.next();
    uassert(7196708, "first argument must be a string", filePathElem.type() == BSONType::string);

    auto fileContentElem = it.next();
    uassert(
        7196707, "second argument must be a BSON array", fileContentElem.type() == BSONType::array);

    const boost::filesystem::path originalFilePath{filePathElem.String()};
    const boost::filesystem::path normalizedFilePath{originalFilePath.lexically_normal()};
    const boost::filesystem::path absoluteFilePath{boost::filesystem::absolute(normalizedFilePath)};

    uassert(7196706,
            "bsonArrayToFile() can only write a file in a directory which already exists",
            boost::filesystem::exists(absoluteFilePath.parent_path()));
    uassert(7196705,
            "bsonArrayToFile() can only write to a file which does not yet exist",
            !boost::filesystem::exists(absoluteFilePath));
    uassert(7196704,
            "the file name must be compatible with POSIX and Windows",
            boost::filesystem::portable_name(absoluteFilePath.filename().string()));

    std::ios::openmode mode = std::ios::out | std::ios::binary;
    boost::filesystem::ofstream ofs{absoluteFilePath, mode};
    uassert(7196703,
            str::stream() << "failed to open file " << normalizedFilePath.string()
                          << " for writing",
            ofs);

    for (const auto& obj : fileContentElem.Obj()) {
        ofs.write(obj.Obj().objdata(), obj.objsize());
        uassert(7196702, "Error writing to file", !ofs.bad());
    }
    ofs.flush();
    uassert(7196701, str::stream() << "failed to write to file " << absoluteFilePath.string(), ofs);

    return undefinedReturn;
}

// The name of the file to dump is provided as a string in the first
// field of the 'a' object. Other arguments in the BSONObj are
// ignored. The void* argument is unused.
BSONObj readDumpFile(const BSONObj& a, void*) {
    uassert(31404,
            "readDumpFile() takes one argument: the path to a file",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);

    // Open the file for reading in binary mode.
    const auto pathStr = a.firstElement().String();
    boost::filesystem::ifstream stream(pathStr, std::ios::in | std::ios::binary);
    uassert(31405,
            str::stream() << "readDumpFile(): Unable to open file \"" << pathStr
                          << "\" for reading",
            stream);

    // Consume the contents of the file into a std::string, or bail out
    // if there is more data in the file or stream than we can handle.
    std::string contents;
    while (stream) {
        char buffer[4096];
        stream.read(buffer, sizeof(buffer));
        contents.append(buffer, stream.gcount());

        // Check that the size of the data can fit into the BSON shape
        // { "" : [ ... ] }, which has 12 bytes of overhead.
        uassert(31406,
                str::stream() << "readDumpFile(): file \"" << pathStr
                              << "\" too big to load as a variable",
                contents.size() <= (BSONObj::DefaultSizeTrait::MaxSize - 12));
    }

    // Construct our return shape
    BSONObjBuilder builder;
    BSONArrayBuilder array(builder.subarrayStart(""));

    // Walk the data we read out of the file and interpret it as a series
    // of contiguous BSON objects. Validate the BSON objects we find and insert
    // them into the results array.
    ConstDataRangeCursor cursor(contents.data(), contents.size());
    while (!cursor.empty()) {

        // Record the amount of valid data ahead of us before
        // advancing the cursor so we can use it as an argument to
        // validate below. It would be nice and proper to use
        // Validated<BSONObj> for all of this instead, but
        // unfortunately the BSONObj specialization of Validated
        // depends on a server parameter, so we do it manually.
        const auto expectedValidBytes = cursor.length();

        const auto swObj = cursor.readAndAdvanceNoThrow<BSONObj>();
        uassertStatusOK(swObj);

        const auto obj = swObj.getValue();
        uassertStatusOKWithContext(validateBSON(obj.objdata(), expectedValidBytes),
                                   str::stream() << " at offset " << cursor.debug_offset());

        array.append(obj);
    }

    array.doneFast();
    return builder.obj();
}

// Returns the number of BSON objects present in a dump file.
// The name of the file to scan is provided as a string in the first
// field of the 'a' object. Other arguments in the BSONObj are
// ignored. The void* argument is unused.
BSONObj numObjsInDumpFile(const BSONObj& a, void*) {
    uassert(9806101,
            "numObjsInDumpFile() takes one argument: the path to a file",
            a.nFields() == 1 && a.firstElementType() == BSONType::string);

    // Open the file for reading in binary mode.
    const auto pathStr = a.firstElement().String();
    boost::filesystem::ifstream stream(pathStr, std::ios::in | std::ios::binary);
    uassert(9806102,
            str::stream() << "numObjsInDumpFile(): Unable to open file \"" << pathStr
                          << "\" for reading",
            stream);

    // Consume the contents of the file into a std::string, or bail out
    // if there is more data in the file or stream than we can handle.
    std::string contents;
    while (stream) {
        char buffer[4096];
        stream.read(buffer, sizeof(buffer));
        contents.append(buffer, stream.gcount());
    }

    // Walk the data we read out of the file and interpret it as a series
    // of contiguous BSON objects. Count the objects without doing validation.
    ConstDataRangeCursor cursor(contents.data(), contents.size());
    int numObjs = 0;
    while (!cursor.empty()) {
        const auto swObj = cursor.readAndAdvanceNoThrow<BSONObj>();
        uassertStatusOK(swObj);
        numObjs++;
    }

    return BSON("" << numObjs);
}

// Returns the nth (0-indexed) object in a dump file.
// The name of the file to scan is provided as a string in the first
// field of the 'a' object. The only other argument should be an integer
// specifying which object to fetch.
BSONObj getObjInDumpFile(const BSONObj& a, void*) {
    uassert(9806103,
            "getObjInDumpFile() takes two arguments: the path to a file and the index of the "
            "object to be fetched",
            a.nFields() == 2 && a.firstElementType() == BSONType::string);

    // Open the file for reading in binary mode.
    BSONObjIterator it(a);

    const std::string pathStr = it.next().str();
    const int objIndex = it.next().safeNumberInt();

    boost::filesystem::ifstream stream(pathStr, std::ios::in | std::ios::binary);
    uassert(9806104,
            str::stream() << "getObjInDumpFile(): Unable to open file \"" << pathStr
                          << "\" for reading",
            stream);

    // Consume the contents of the file into a std::string, or bail out
    // if there is more data in the file or stream than we can handle.
    std::string contents;
    while (stream) {
        char buffer[4096];
        stream.read(buffer, sizeof(buffer));
        contents.append(buffer, stream.gcount());
    }

    // Walk the data we read out of the file and interpret it as a series
    // of contiguous BSON objects. Skip over objIndex objects and return
    // the next one.
    ConstDataRangeCursor cursor(contents.data(), contents.size());
    for (int i = 0; i < objIndex && !cursor.empty(); ++i) {
        // We do not perform validation on BSONObj
        const auto swObj = cursor.readAndAdvanceNoThrow<BSONObj>();
        uassertStatusOK(swObj);
    }
    uassert(9806105,
            str::stream() << "getObjInDumpFile(): File does not contain an object at "
                          << "index " << objIndex,
            !cursor.empty());

    // Record the amount of valid data ahead of us before
    // advancing the cursor so we can use it as an argument to
    // validate below. It would be nice and proper to use
    // Validated<BSONObj> for all of this instead, but
    // unfortunately the BSONObj specialization of Validated
    // depends on a server parameter, so we do it manually.
    const auto expectedValidBytes = cursor.length();
    BSONObj obj;
    cursor.readAndAdvance<BSONObj>(&obj);
    uassertStatusOKWithContext(validateBSON(obj.objdata(), expectedValidBytes),
                               str::stream() << " at offset " << cursor.debug_offset());

    BSONObjBuilder builder;
    builder.append("", obj);
    return builder.obj<BSONObj::LargeSizeTrait>();
}

// Returns the number of BSON objects present in the BSON corpus.
// This is a corpus of edge cases and interesting BSON objects
// intended to be used for testing
BSONObj numObjsInCorpus(const BSONObj& a, void*) {
    uassert(9479201, "numObjsInCorpus accepts no arguments", a.nFields() == 0);

    return BSON("" << mongo::bson::corpusSize());
}

// Returns the nth (0-indexed) object in the BSON corpus.
// The name of the file to scan is provided as a string in the first
// field of the 'a' object. The only other argument should be an integer
// specifying which object to fetch.
BSONObj getObjInCorpus(const BSONObj& a, void*) {
    uassert(9479202,
            "getObjInCorpus() takes one argument: the index of the "
            "object to be fetched",
            a.nFields() == 1);

    BSONObjIterator it(a);
    const int objIndex = it.next().safeNumberInt();
    auto contents = mongo::bson::getCorpusObject(objIndex);

    ConstDataRangeCursor cursor(contents.data(), contents.size());
    const auto expectedValidBytes = cursor.length();
    BSONObj obj;
    cursor.readAndAdvance<BSONObj>(&obj);
    uassertStatusOKWithContext(validateBSON(obj.objdata(), expectedValidBytes),
                               str::stream() << " at offset " << cursor.debug_offset());

    BSONObjBuilder builder;
    builder.append("", obj);
    return builder.obj<BSONObj::LargeSizeTrait>();
}

BSONObj ls(const BSONObj& args, void* data) {
    BSONArrayBuilder ret;
    BSONObj o = listFiles(args, data);
    if (!o.isEmpty()) {
        for (auto&& elem : o.firstElement().Obj()) {
            BSONObj f = elem.Obj();
            string name = f["name"].String();
            if (f["isDirectory"].trueValue()) {
                name += '/';
            }
            ret << name;
        }
    }
    return BSON("" << ret.arr());
}

BSONObj removeFile(const BSONObj& args, void* data) {
    BSONElement e = singleArg(args);
    bool found = false;

    boost::filesystem::path root(e.str());
    if (boost::filesystem::exists(root)) {
        found = true;
        boost::filesystem::remove_all(root);
    }

    BSONObjBuilder b;
    b.appendBool("removed", found);
    return b.obj();
}

void installShellUtilsExtended(Scope& scope) {
    scope.injectNative("getHostName", getHostName);
    scope.injectNative("removeFile", removeFile);
    scope.injectNative("copyFile", copyFile);
    scope.injectNative("appendFile", appendFile);
    scope.injectNative("writeFile", writeFile);
    scope.injectNative("listFiles", listFiles);
    scope.injectNative("ls", ls);
    scope.injectNative("pwd", pwd);
    scope.injectNative("cd", cd);
    scope.injectNative("cat", cat);
    scope.injectNative("hostname", hostname);
    scope.injectNative("md5sumFile", md5sumFile);
    scope.injectNative("mkdir", mkdir);
    scope.injectNative("copyDir", copyDir);
    scope.injectNative("passwordPrompt", passwordPrompt);
    scope.injectNative("umask", changeUmask);
    scope.injectNative("getFileMode", getFileMode);
    scope.injectNative("decompressBSONColumn", decompressBSONColumn);
    scope.injectNative("dumpBSONAsHex", dumpBSONAsHex);
    scope.injectNative("hexToBSON", hexToBSON);
    scope.injectNative("_copyFileRange", copyFileRange);
    scope.injectNative("_readDumpFile", readDumpFile);
    scope.injectNative("_numObjsInDumpFile", numObjsInDumpFile);
    scope.injectNative("_getObjInDumpFile", getObjInDumpFile);
    scope.injectNative("_numObjsInCorpus", numObjsInCorpus);
    scope.injectNative("_getObjInCorpus", getObjInCorpus);
    scope.injectNative("_getEnv", shellGetEnv);
    scope.injectNative("writeBsonArrayToFile", writeBsonArrayToFile);
    scope.injectNative("getStringWidth", getStringWidth);
    scope.injectNative("_generateStorageBSON", generateStorageBSON);
}

}  // namespace shell_utils
}  // namespace mongo
