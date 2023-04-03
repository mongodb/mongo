/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "io_trace.h"
#include "util.h"

/*
 * io_trace::io_trace --
 *     Initialize a new io_trace object.
 */
io_trace::io_trace(io_trace_collection &parent, const char *name) : _name(name), _parent(parent) {}

/*
 * io_trace::~io_trace --
 *     Destroy a io_trace object.
 */
io_trace::~io_trace() {}

/*
 * io_trace_collection::io_trace_collection --
 *     Initialize a new trace collection.
 */
io_trace_collection::io_trace_collection() {}

/*
 * io_trace_collection::~io_trace_collection --
 *     Destroy a collection of traces.
 */
io_trace_collection::~io_trace_collection()
{
    for (auto &e : _traces) {
        delete e.second;
    }
}

/*
 * io_trace_collection::add --
 *     Add a data point.
 */
void
io_trace_collection::add_data_point(
  const std::string &device_or_file, const io_trace_kind kind, const io_trace_operation &item)
{
    std::string name = device_or_file;

    /* Postprocess the data point. */

    if (kind == io_trace_kind::WIRED_TIGER) {
        if (name.find("Log.00000") != std::string::npos ||
          name.find("log.00000") != std::string::npos)
            name = "WiredTiger Logs (combined)";
        else if (ends_with(name, "WiredTiger.wt"))
            name = "WiredTiger Main File: " + name;
        else if (ends_with(name, "WiredTigerHS.wt"))
            name = "WiredTiger Main File: " + name;
        else if (ends_with(name, ".wt"))
            name = "WiredTiger File: " + name;
        else
            name = "WiredTiger Other";
    }

    if (kind == io_trace_kind::DEVICE)
        name = "Raw Device: " + name;

    /* Find the correct trace. */

    auto ti = _traces.find(name);
    io_trace *t;
    if (ti == _traces.end()) {
        t = new io_trace(*this, name.c_str());
        _traces[name] = t;
    } else
        t = ti->second;

    /* Insert the item into the correct place to keep the list sorted. */

    if (t->_operations.empty())
        t->_operations.push_back(item);
    else {
        const io_trace_operation &last = t->_operations.back();
        if (last.timestamp <= item.timestamp)
            t->_operations.push_back(item);
        else
            t->_operations.insert(
              std::upper_bound(t->_operations.begin(), t->_operations.end(), item), item);
    }
}

/*
 * io_trace_collection::load_from_file --
 *     Load a collection from file.
 */
void
io_trace_collection::load_from_file(const std::string &file)
{
    FILE *f = fopen(file.c_str(), "r");
    if (!f)
        throw std::runtime_error(
          std::string("Error opening file \"") + file + "\": " + strerror(errno));

    try {
        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            rewind(f);
            /* Guess the file type from the first line */
            if (isdigit(buf[0]))
                load_from_file_blkparse(f);
            else if (buf[0] == '[')
                load_from_file_wt_logs(f);
            else
                throw std::runtime_error("Unsupported file type");
        }
    } catch (std::exception &e) {
        (void)fclose(f);
        throw std::runtime_error(std::string("Error while loading \"") + file + "\": " + e.what());
    }

    if (fclose(f) != 0)
        throw std::runtime_error(
          std::string("Error closing file \"") + file + "\": " + strerror(errno));
}

#define MAX_TOKENS_PER_LINE 16

/*
 * io_trace_collection::load_from_file_blkparse --
 *     Load from the blkparse output, using default settings, optionally with the -t argument.
 */
void
io_trace_collection::load_from_file_blkparse(FILE *f)
{
    /*
     * Sample output with the default settings from "blkparse -t":
     *
     * 259,1   14        2     0.001744322     0  C  WS 365166592 + 8 (  561300) [0]
     * 259,1    6       46     0.196381562 29456  D  WS 683941352 + 512 (196381562) [wtperf]
     *
     * The fields are:
     *   0) Device
     *   1) CPU number
     *   2) Sequence number (within the given CPU)
     *   3) Timestamp
     *   4) PID
     *   5) Action (e.g., D = issue, C = completion)
     *   6) The RWBS field (R = read, W = write, B = barrier, S = synchronous)
     *   7) Block offset (using 512-byte blocks)
     *   8) Always "+"
     *   9) Block length
     *  10) Duration in (), in microseconds (enabled via the "-t" command-line argument)
     *  11) Process name and potentally other process-relevant information in []
     *
     * Note that actions other than C and D can carry different extra information on fields 7
     * and up.
     */

    char buf[256];
    unsigned int lines = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lines++;
        if (strncmp(buf, "CPU0", 4) == 0)
            break;

        /* Tokenize the line. */

        int count = 0;
        char *parts[MAX_TOKENS_PER_LINE];
        char *next(buf), *p;
        while ((p = strsep(&next, " \t\n\r")) != NULL) {
            if (*p == '\0')
                continue;
            parts[count++] = p;
            if (count >= MAX_TOKENS_PER_LINE)
                break;
        }

        if (count <= 5)
            throw std::runtime_error(std::string("Incomplete line ") + std::to_string(lines));

        /* For now, we only care about the complete action (C). */

        char log_action = *(parts[5]);
        if (log_action != 'C')
            continue;

        if (count <= 10)
            throw std::runtime_error(std::string("Incomplete line ") + std::to_string(lines));

        /* Parse the common line data. */

        char *e;
        io_trace_operation item;

        std::string device = parts[0];

        item.timestamp = strtod(parts[3], &e);
        if (*e != '\0')
            throw std::runtime_error(
              std::string("Cannot parse the timestamp on line ") + std::to_string(lines));

        item.action = log_action;
        for (e = parts[6]; *e != '\0'; e++) {
            switch (*e) {
            case 'R':
                item.read = true;
                break;
            case 'W':
                item.write = true;
                break;
            case 'S':
                item.synchronous = true;
                break;
            case 'B':
                item.barrier = true;
                break;
            case 'D':
                item.discard = true;
                break;
            }
        }

        /* Parse the offset and the length. */

        item.offset = strtol(parts[7], &e, 10);
        if (*e != '\0')
            throw std::runtime_error(
              std::string("Cannot parse the offset on line ") + std::to_string(lines));

        if (strcmp(parts[8], "+") != 0)
            throw std::runtime_error(
              std::string("Unexpected payload on line ") + std::to_string(lines));

        item.length = strtol(parts[9], &e, 10);
        if (*e != '\0')
            throw std::runtime_error(
              std::string("Cannot parse the length on line ") + std::to_string(lines));

        item.offset *= 512;
        item.length *= 512;

        /* Parse the other action-specific data. */

        int index = 10;
        p = parts[index];

        if (index < count && *p == '(') {
            p++;
            if (*p == '\0')
                p = parts[++index];
            if (index < count) {
                p[strlen(p) - 1] = '\0';
                item.duration = strtol(parts[index], &e, 10) / 1.0e6;
                if (item.duration > 1e3)
                    item.duration = 0;
                p = parts[++index];
            }
        }

        if (index < count && *p == '[') {
            p++;
            p[strlen(p) - 1] = '\0';
            e = strrchr(p, '/');
            if (e != NULL && isdigit(e[1]))
                *e = '\0';
            strncpy(item.process, p, sizeof(item.process));
            p = parts[++index];
        }

        /* Right now we only care about reads and writes. */

        if (!item.read && !item.write)
            continue;

        /* Postprocess and save the line. */

        add_data_point(device, io_trace_kind::DEVICE, item);
    }
}

/*
 * io_trace_collection::load_from_file_wt_logs --
 *     Load a file-level I/O trace from the WiredTiger logs.
 */
void
io_trace_collection::load_from_file_wt_logs(FILE *f)
{
    /*
     * Sample logs (with line breaks added):
     *
     * [1678914463:707985][29429:0x7f532741cc40], WT_SESSION.create: [WT_VERB_WRITE][DEBUG_2]:
     *   write: /data/wt/test.wt, fd=10, offset=0, len=4096
     * [1678914463:711029][29429:0x7f531effd640], log-server: [WT_VERB_WRITE][DEBUG_2]:
     *    write: /data/wt/./WiredTigerTmplog.0000000003, fd=9, offset=0, len=128
     * [1678914463:712302][29429:0x7f532741cc40], file:test.wt, WT_SESSION.create:
     *   [WT_VERB_READ][DEBUG_2]: read: /data/wt/test.wt, fd=10, offset=0, len=4096
     * [1678914463:713343][29429:0x7f532741cc40], file:WiredTiger.wt, WT_SESSION.create:
     *   [WT_VERB_WRITE][DEBUG_2]: write: /data/wt/./WiredTigerLog.0000000001, fd=7,
     * offset=1920, len=2048 [1678914463:715599][29429:0x7f53157fa640]:
     * [WT_VERB_WRITE][DEBUG_2]: write: /data/wt/./WiredTigerLog.0000000001, fd=7, offset=3968,
     * len=131328
     */

    char buf[256];
    double first_timestamp = 0;
    unsigned int lines = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lines++;
        if (buf[0] != '[')
            continue;

        /* Tokenize the line. */

        int count = 0;
        char *parts[MAX_TOKENS_PER_LINE];
        char *next(buf), *p;
        while ((p = strsep(&next, " \t\n\r")) != NULL) {
            if (*p == '\0')
                continue;
            size_t len = strlen(p);
            if (p[len - 1] == ':' || p[len - 1] == ',')
                p[len - 1] = '\0';
            parts[count++] = p;
            if (count >= MAX_TOKENS_PER_LINE)
                break;
        }

        if (count == 0)
            continue;

        /* Find the read and write verbs + the debug level. */

        bool read = false;
        bool write = false;
        int index = 0;
        while (++index < count) {
            if (strcmp(parts[index], "[WT_VERB_READ][DEBUG_2]") == 0) {
                read = true;
                break;
            }
            if (strcmp(parts[index], "[WT_VERB_WRITE][DEBUG_2]") == 0) {
                write = true;
                break;
            }
        }

        if ((!read && !write) || index + 1 >= count)
            continue;

        if (read &&
          (strcmp(parts[index + 1], "read") != 0 && strcmp(parts[index + 1], "read-mmap") != 0))
            continue;

        if (write &&
          (strcmp(parts[index + 1], "write") != 0 && strcmp(parts[index + 1], "write-mmap") != 0))
            continue;

        /* Parse the line, starting with the timestamps. */

        char *e;
        io_trace_operation item;

        item.timestamp = strtol(parts[0] + 1, &e, 10);
        if (*e != ':')
            throw std::runtime_error(
              std::string("Cannot parse the timestamp on line ") + std::to_string(lines));
        item.timestamp += strtol(e + 1, &e, 10) / 1.0e+6;
        if (*e != ']')
            throw std::runtime_error(
              std::string("Cannot parse the timestamp on line ") + std::to_string(lines));

        if (first_timestamp == 0)
            first_timestamp = item.timestamp;
        item.timestamp -= first_timestamp;

        /* Parse the read/write info. */

        item.action = 'D'; /* "Issue" in blkparse. */
        item.read = read;
        item.write = write;

        if (index + 5 >= count)
            throw std::runtime_error(std::string("Incomplete line ") + std::to_string(lines));

        std::string path = parts[index + 2];

        if (strncmp(parts[index + 4], "offset=", 7) != 0 ||
          strncmp(parts[index + 5], "len=", 4) != 0)
            throw std::runtime_error(
              std::string("Unexpected payload on line ") + std::to_string(lines));

        item.offset = strtol(parts[index + 4] + 7, &e, 10);
        if (*e != '\0')
            throw std::runtime_error(
              std::string("Cannot parse the offset on line ") + std::to_string(lines));

        item.length = strtol(parts[index + 5] + 4, &e, 10);
        if (*e != '\0')
            throw std::runtime_error(
              std::string("Cannot parse the length on line ") + std::to_string(lines));

        /* Postprocess and save the line. */

        add_data_point(path, io_trace_kind::WIRED_TIGER, item);
    }
}
