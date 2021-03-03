#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import argparse
import colorsys
from multiprocessing import Process
import multiprocessing
import os
import os.path
import struct
import sys
import subprocess
import time
import traceback

#
# This log version must be the same as that defined in ../src/include/optrack.h
#
currentLogVersion = 2;

class color:
   PURPLE = '\033[95m'
   CYAN = '\033[96m'
   DARKCYAN = '\033[36m'
   BLUE = '\033[94m'
   GREEN = '\033[92m'
   YELLOW = '\033[93m'
   RED = '\033[91m'
   BOLD = '\033[1m'
   UNDERLINE = '\033[4m'
   END = '\033[0m'

functionMap = {};

def buildTranslationMap(mapFileName):

    mapFile = None;

    if not os.path.exists(mapFileName):
        return False;

    try:
        mapFile = open(mapFileName, "r");
    except:
        print(color.BOLD + color.RED);
        print("Could not open " + mapFileName + " for reading");
        print(color.END);
        raise;

    # Read lines from the map file and build an in-memory map
    # of translations. Each line has a function ID followed by space and
    # followed by the function name.
    #
    lines = mapFile.readlines();  # a map file is usually small

    for line in lines:

        words = line.split(" ");
        if (len(words) < 2):
            continue;

        try:
            funcID = int(words[0]);
        except:
            continue;

        funcName = words[1].strip();

        functionMap[funcID] = funcName;

    return True;

def funcIDtoName(funcID):

    if funcID in functionMap:
        return functionMap[funcID];
    else:
       print("Could not find the name for func " + str(funcID));
       return "NULL";

#
# The format of the record is written down in src/include/optrack.h
# file in the WiredTiger source tree. The current implementation assumes
# a record of three fields. The first field is the 8-byte timestamp.
# The second field is the 2-byte function ID. The third field is the
# 2-byte operation type: '0' for function entry, '1' for function exit.
# The record size would be padded to 16 bytes in the C implementation by
# the compiler, because we keep an array of records, and each new record
# has to be 8-byte aligned, since the first field has the size 8 bytes.
# So we explicitly pad the track record structure in the implementation
# to make it clear what the record size is.
#
def parseOneRecord(file):

    bytesRead = "";
    record = ();
    RECORD_SIZE = 16;

    try:
        bytesRead = file.read(RECORD_SIZE);
    except:
        return None;

    if (len(bytesRead) < RECORD_SIZE):
        return None;

    record = struct.unpack('Qhhxxxx', bytesRead);

    return record;

#
# HEADER_SIZE must be the same as the size of WT_OPTRACK_HEADER
# structure defined in ../src/include/optrack.h
#
def validateHeader(file):

    global currentLogVersion;

    bytesRead = "";
    MIN_HEADER_SIZE = 12;

    try:
        bytesRead = file.read(MIN_HEADER_SIZE);
    except:
        print(color.BOLD + color.RED +
              "failed read of input file" + color.END);
        raise;

    if (len(bytesRead) < MIN_HEADER_SIZE):
        print(color.BOLD + color.RED +
              "unexpected sized input file" + color.END);
        raise;

    version, threadType, tsc_nsec = struct.unpack('=III', bytesRead);
    print("VERSION IS " + str(version));

    # If the version number is 2, the header contains three fields:
    # version, thread type, and clock ticks per nanosecond).
    # If the version number is 3 or greater, the header also contains
    # field: an 8-byte timestamp in seconds since the Epoch, as
    # would be returned by a call to time() on Unix.
    #
    if (version == 2):
        return True, threadType, tsc_nsec, 0;
    elif(version >= 3):
        ADDITIONAL_HEADER_SIZE = 12;
        try:
            bytesRead = file.read(ADDITIONAL_HEADER_SIZE);
            if (len(bytesRead) < ADDITIONAL_HEADER_SIZE):
                return False, -1;

            padding, sec_from_epoch = struct.unpack('=IQ', bytesRead);
            return True, threadType, tsc_nsec, sec_from_epoch;
        except:
            return False, -1;
    else:
        return False, -1, 1;

def getStringFromThreadType(threadType):

    if (threadType == 0):
        return "external";
    elif (threadType == 1):
        return "internal";
    else:
        return unknown;


def parseFile(fileName):

    done = False;
    file = None;
    threadType = 0;
    threadTypeString = None;
    tsc_nsec_ratio = 1.0;
    outputFile = None;
    outputFileName = "";
    totalRecords = 0;
    validVersion = False;

    print(color.BOLD + "Processing file " + fileName + color.END);

    # Open the log file for reading
    try:
        file = open(fileName, "rb");
    except:
        print(color.BOLD + color.RED +
              "Could not open " + fileName + " for reading" + color.END);
        raise;

    # Read and validate log header
    validVersion, threadType, tsc_nsec_ratio, sec_from_epoch = \
                                                    validateHeader(file);
    if (not validVersion):
        return;

    # Find out if this log file was generated by an internal or an
    # external thread. This will be reflected in the output file name.
    #
    threadTypeString = getStringFromThreadType(threadType);

    # This ratio tells us how many clock ticks there are in a nanosecond
    # on the processor on which this trace file was generated. When the WT
    # library logs this ratio, it multiplies it by 1000. So we have to divide
    # it back to get an accurate ratio.
    tsc_nsec_ratio = float(tsc_nsec_ratio) / 1000.0;

    print("TSC_NSEC ratio parsed: " + '{0:,.4f}'.format(tsc_nsec_ratio));

    # Open the text file for writing
    try:
        outputFileName = fileName + "-" + threadTypeString + ".txt";
        outputFile = open(outputFileName, "w");
    except:
        print(color.BOLD + color.RED +
              "Could not open file " + outputfileName + ".txt for writing." +
              color.END);
        return;

    print(color.BOLD + color.PURPLE +
          "Writing to output file " + outputFileName + "." + color.END);

    # The first line of the output file contains the seconds from Epoch
    outputFile.write(str(sec_from_epoch) + "\n");

    while (not done):
        record = parseOneRecord(file);

        if ((record is None) or len(record) < 3):
            done = True;
        else:
            try:
                time = float(record[0]) / tsc_nsec_ratio;
                funcName = funcIDtoName(record[1]);
                opType = record[2];

                outputFile.write(str(opType) + " " + funcName + " "
                                 + str(int(time))
                                 + "\n");
                totalRecords += 1;
            except:
                exc_type, exc_value, exc_traceback = sys.exc_info()
                traceback.print_exception(exc_type, exc_value, exc_traceback);
                print(color.BOLD + color.RED);
                print("Could not write record " + str(record) +
                      " to file " + fileName + ".txt.");
                print(color.END);
                done = True;

    print("Wrote " + str(totalRecords) + " records to " + outputFileName + ".");
    file.close();
    outputFile.close();

def waitOnOneProcess(runningProcesses):

    success = False;
    # Use a copy since we will be deleting entries from the original
    for fname, p in runningProcesses.copy().items():
        if (not p.is_alive()):
            del runningProcesses[fname];
            success = True;

    # If we have not found a terminated process, sleep for a while
    if (not success):
        time.sleep(5);

def main():

    runnableProcesses = {};
    spawnedProcesses = {};
    successfullyProcessedFiles = [];
    targetParallelism = multiprocessing.cpu_count();
    terminatedProcesses = {};

    parser = argparse.ArgumentParser(description=
                                     'Convert WiredTiger operation \
                                     tracking logs from binary to \
                                     text format.');

    parser.add_argument('files', type=str, nargs='*',
                    help='optrack log files to process');

    parser.add_argument('-j', dest='jobParallelism', type=int,
                        default='0');

    parser.add_argument('-m', '--mapfile', dest='mapFileName', type=str,
                        default='optrack-map');

    args = parser.parse_args();

    print("Running with the following parameters:");
    for key, value in vars(args).items():
        print ("\t" + key + ": " + str(value));

    # Parse the map of function ID to name translations.
    if (buildTranslationMap(args.mapFileName) is False):
        print("Failed to locate or parse the map file " +
              args.mapFileName);
        print("Cannot proceed.");
        return;

    # Determine the target job parallelism
    if (args.jobParallelism > 0):
        targetParallelism = args.jobParallelism;
    if (targetParallelism == 0):
        targetParallelism = len(args.files);
    print(color.BLUE + color.BOLD +
          "Will process " + str(targetParallelism) + " files in parallel."
          + color.END);

    # Prepare the processes that will parse files, one per file
    if (len(args.files) > 0):
        for fname in args.files:
            p = Process(target=parseFile, args=(fname,));
            runnableProcesses[fname] = p;

    # Spawn these processes, not exceeding the desired parallelism
    while (len(runnableProcesses) > 0):
        while (len(spawnedProcesses) < targetParallelism
               and len(runnableProcesses) > 0):

            fname, p = runnableProcesses.popitem();
            p.start();
            spawnedProcesses[fname] = p;

        # Find at least one terminated process
        waitOnOneProcess(spawnedProcesses);

    # Wait for all processes to terminate
    while (len(spawnedProcesses) > 0):
        waitOnOneProcess(spawnedProcesses);

if __name__ == '__main__':
    main()
