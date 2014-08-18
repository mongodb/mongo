/*
 *    Copyright 2014 MongoDB Inc.
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

#include <algorithm>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/scoped_ptr.hpp>
#include <fstream>
#include <numeric>
#include <string>

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers_synchronous.h"

using namespace mongo;

namespace {
    namespace file = boost::filesystem;
    namespace time = boost::posix_time;

    typedef unsigned long long bytes_t;
    typedef long long micros_t;

    const long DEFAULT_FILE_SIZE_MB =  128;
    const file::path DEFAULT_PATH = file::temp_directory_path();
    const int DEFAULT_NTRIALS = 10;
    const bool DEFAULT_BSON_OUT = false;

    // used to convert B/usec to MB/sec
    const double MICROSEC_PER_SEC = 1e6;
    const double MB_PER_BYTE =  1.0 / (1 << 20);
    const double MB_SEC_CONVERSION_FACTOR = MICROSEC_PER_SEC * MB_PER_BYTE;

    double toMbSec(const bytes_t bytes, const micros_t micros) {
        return (static_cast<double>(bytes) / static_cast<double>(micros)) *
            MB_SEC_CONVERSION_FACTOR;
    }
}

struct BenchmarkParams {
    bytes_t bytes;
    file::path path;
    int ntrials;
    bool quiet;
    bool jsonReportEnabled;
    std::string jsonReportOut;
} benchParams;

class FileAllocatorBenchmark {
public:
    FileAllocatorBenchmark(const BenchmarkParams& params)
        : _fa(FileAllocator::get())
        , _params(params) {
        _fa->start();

        if (!file::create_directory(_params.path)) {
            std::cerr << "Error: unable to create temporary directory in "
                      << _params.path.parent_path() << std::endl;
            ::_exit(EXIT_FAILURE);
        }
    }

    // Delete any files we created
    ~FileAllocatorBenchmark() {
        file::remove_all(_params.path);
    }

    void run() {
        if (!_params.quiet) {
            std::cout << "Allocating " << _params.ntrials << " files of size "
                      << _params.bytes << " bytes in " << _params.path << std::endl;
        }

        for (int n = 0; n < _params.ntrials; ++n) {
            const std::string fileName = str::stream() << "garbage-" << n;
            file::path filePath = _params.path / fileName;
            _files.push_back(filePath);
            bytes_t size_allocated = _params.bytes;
            const time::ptime start = time::microsec_clock::universal_time();

            _fa->allocateAsap(filePath.string(), size_allocated);

            if (size_allocated != static_cast<bytes_t>(_params.bytes)) {
                std::cerr << "Allocated " << size_allocated << " bytes but expected "
                          << _params.bytes;
            }

            const time::ptime end = time::microsec_clock::universal_time();
            _results.push_back((end - start).total_microseconds());
        }

        _fa->waitUntilFinished();

        if (!_params.quiet) {
            textReport();
        }

        if (_params.jsonReportEnabled) {
            jsonReport(_params.jsonReportOut);
        }
    }

private:
    struct benchResults {
        micros_t avg;
        micros_t max;
        micros_t min;
    };

    benchResults computeResults() {
        benchResults res;
        const micros_t total = std::accumulate(_results.begin(), _results.end(), 0L);
        res.avg = total / _results.size();
        res.max = *std::max_element(_results.begin(), _results.end());
        res.min = *std::min_element(_results.begin(), _results.end());
        return res;
    }

    void printResult(const std::string& name, const micros_t duration, const bytes_t bytes) {
        std::cout << name << ": " << duration << " usec = "
                  << toMbSec(bytes, duration) << " MB/sec" << std::endl;

    }

    void textReport() {
        benchResults results = computeResults();

        std::cout << "Results for " << _params.ntrials << " allocations of "
                  << _params.bytes << " bytes: " << std::endl;

        printResult("avg", results.avg, _params.bytes);
        printResult("max", results.max, _params.bytes);
        printResult("min", results.min, _params.bytes);
    }

    void addResult(BSONObjBuilder& obj, const std::string& name,
                   const micros_t duration, const bytes_t bytes) {
        BSONObjBuilder so(obj.subobjStart(name));
        so.append("usec", duration);
        so.append("MBsec", toMbSec(bytes, duration));
        so.done();
    }

    void jsonReport(const std::string& jsonReportOut) {
        benchResults results = computeResults();
        BSONObjBuilder obj;

        obj.append("bytes", static_cast<long long>(_params.bytes));
        addResult(obj, "avg", results.avg, _params.bytes);
        addResult(obj, "max", results.max, _params.bytes);
        addResult(obj, "min", results.min, _params.bytes);

        obj.append("raw", _results);

        const std::string outStr = obj.done().toString();

        if (jsonReportOut == "-") {
            std::cout << outStr << std::endl;
        } else {
            std::ofstream outfile(jsonReportOut.c_str());
            if (!outfile.is_open()) {
                std::cerr << "Error: couldn't create output file " << jsonReportOut << std::endl;
                return;
            }
            ON_BLOCK_EXIT(&std::ofstream::close, outfile);
            outfile << outStr << std::endl;
        }
    }

    FileAllocator* const _fa;
    std::vector<micros_t> _results;
    std::vector<file::path> _files;

    const BenchmarkParams& _params;
};

namespace moe = mongo::optionenvironment;

Status addFileAllocatorBenchOptions(moe::OptionSection& options) {
    options.addOptionChaining("help", "help", moe::Switch, "Display help");
    options.addOptionChaining("megabytes", "megabytes", moe::Long,
                              "The number of megabytes to allocate for each file")
        .setDefault(moe::Value(DEFAULT_FILE_SIZE_MB));

    options.addOptionChaining("path", "path", moe::String,
                              str::stream() << "The directory path to allocate the file(s) in "
                                            << "during testing. Files will be allocated in a "
                                            << "uniquely named temporary directory within the "
                                            << "specified path")
        .setDefault(moe::Value(DEFAULT_PATH.string()));

    options.addOptionChaining("ntrials", "ntrials", moe::Int,
                              "The number of trials to perform")
        .setDefault(moe::Value(DEFAULT_NTRIALS));

    options.addOptionChaining("quiet", "quiet", moe::Switch,
                              "Suppress the plaintext report");

    options.addOptionChaining("jsonReport", "jsonReport", moe::String,
                              str::stream() << "If set, results will be saved as a JSON document to "
                                            << "the specified file path. If specified with no "
                                            << "arguments the report will be printed to standard "
                                            << "out")
        .setImplicit(moe::Value(std::string("-")));

    return Status::OK();
}

Status validateFileAllocatorBenchOptions(const moe::OptionSection& options,
                                         moe::Environment& env) {
    Status ret = env.validate();
    if (!ret.isOK()) {
        return ret;
    }
    bool displayHelp = false;
    ret = env.get(moe::Key("help"), &displayHelp);
    if (displayHelp) {
        std::cout << options.helpString() << std::endl;
        ::_exit(EXIT_SUCCESS);
    }
    return Status::OK();
}

Status storeFileAllocatorBenchOptions(const moe::Environment& env) {
    // don't actually need to check Status since we set default values
    long mbytes;
    Status ret = env.get(moe::Key("megabytes"), &mbytes);
    benchParams.bytes = mbytes * (1 << 20);

    std::string path;
    ret = env.get(moe::Key("path"), &path);

    file::path rootPath = file::path(path);

    if (!file::is_directory(rootPath)) {
        std::cerr << "Error: path argument must be a directory" << std::endl;
        ::_exit(EXIT_FAILURE);
    }

    benchParams.path = rootPath / file::unique_path("allocator-bench-%%%%%%%%");

    ret = env.get(moe::Key("ntrials"), &benchParams.ntrials);
    ret = env.get(moe::Key("quiet"), &benchParams.quiet);

    benchParams.jsonReportEnabled = true;
    ret = env.get(moe::Key("jsonReport"), &benchParams.jsonReportOut);
    if (!ret.isOK()) {
        benchParams.jsonReportEnabled = false;
    }
    return Status::OK();
}

int main(int argc, char** argv, char** envp) {
    ::mongo::setupSynchronousSignalHandlers();
    ::mongo::runGlobalInitializersOrDie(argc, argv, envp);

    FileAllocatorBenchmark(benchParams).run();
    ::_exit(EXIT_SUCCESS);
}

MONGO_INITIALIZER(KillLoggingOutput)(InitializerContext* context) {
    // The FileAllocator produces a lot of log noise, so we silence
    // all logging output
    mongo::logger::globalLogDomain()->clearAppenders();
    return Status::OK();
}

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(FileAllocatorBenchOptions)(InitializerContext* context) {
    return addFileAllocatorBenchOptions(moe::startupOptions);
}

MONGO_STARTUP_OPTIONS_VALIDATE(FileAllocatorBenchOptions)(InitializerContext* context) {
    return validateFileAllocatorBenchOptions(moe::startupOptions,
                                             moe::startupOptionsParsed);
}

MONGO_STARTUP_OPTIONS_STORE(FileAllocatorBenchOptions)(InitializerContext* context) {
    return storeFileAllocatorBenchOptions(moe::startupOptionsParsed);
}
