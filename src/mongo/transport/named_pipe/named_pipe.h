// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#ifndef _WIN32
#include <fstream>
#else
#include <windows.h>
#endif
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/transport/named_pipe/input_object.h"
#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;
#ifndef _WIN32
static constexpr auto kDefaultPipePath = "/tmp/"sv;
#else
// "//./pipe/" is the required path start of all named pipes on Windows, where "//." is the
// abbreviation for the local server name and "/pipe" is a literal. (These also work with
// Windows-native backslashes instead of forward slashes.
static constexpr auto kDefaultPipePath = "//./pipe/"sv;
#endif

class NamedPipeOutput {
public:
    // Searches the named pipe in 'kDefaultPipePath' + 'pipeRelativePath'
    NamedPipeOutput(const std::string& pipeRelativePath)
        : NamedPipeOutput(std::string{kDefaultPipePath}, pipeRelativePath) {}

    // Searches the named pipe in 'pipeDir' + 'pipeRelativePath' in POSIX system'
    NamedPipeOutput(const std::string& pipeDir,
                    const std::string& pipeRelativePath,
                    bool persistPipe = false);

    ~NamedPipeOutput();
    void open();
    int write(const char* data, int size);
    void close();

private:
    std::string _pipeAbsolutePath;
#ifndef _WIN32
    std::ofstream _ofs;
    bool _persistPipe;
#else
    HANDLE _pipe;
    bool _isOpen;
#endif
};

class NamedPipeInput : public StreamableInput {
public:
    NamedPipeInput(const std::string& pipeRelativePath);
    ~NamedPipeInput() override;
    const std::string& getAbsolutePath() const override {
        return _pipeAbsolutePath;
    }
    bool isOpen() const override;
    bool isGood() const override;
    bool isFailed() const override;
    bool isEof() const override;

protected:
    void doOpen() override;
    int doRead(char* data, int size) override;
    void doClose() override;

private:
    std::string _pipeAbsolutePath;
#ifndef _WIN32
    std::ifstream _ifs;
#else
    HANDLE _pipe;
    bool _isOpen : 1;
    bool _isGood : 1;
    bool _isEof : 1;
#endif
};
}  // namespace mongo
