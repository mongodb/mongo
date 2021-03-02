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
#!/usr/bin/env python

import argparse
import multiprocessing
from multiprocessing import Process
import numpy as np
import os
import pandas as pd
import sys
import time

# The time units used in the input files is nanoseconds. Presently the
# operation tracking code does not produce data using any other time
# units.
#
unitsPerSecond = 1000000000;

# We aggregate data for intervals with the duration specified by
# the following variable.
intervalLength = 1;

# Each file has a timestamp indicating when the logging began
perFileTimeStamps = {};

# Codes for various colors for printing of informational and error messages.
#
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

#
# Go over all operation records in the dataframe and assign stack depths.
#
def assignStackDepths(dataframe):

    stack = [];

    df = dataframe.sort_values(by=['start']);
    df = df.reset_index(drop = True);

    for i in range(len(df.index)):

        myEndTime = df.at[i, 'end'];

        # Pop all items off stack whose end time is earlier than my
        # end time. They are not the callers on my stack, so I don't want to
        # count them.
        #
        while (len(stack) > 0 and stack[-1] < myEndTime):
            stack.pop();

        df.at[i, 'stackdepth'] = len(stack);
        stack.append(df.at[i, 'end']);

    return df;

def reportDataError(logfile, logfilename):

    if (logfile is not sys.stdout):
        print(color.BOLD + color.RED + "Your data may have errors. " +
              "Check the file " + logfilename + " for details." + color.END);
    return True;

#
# An intervalEnd is a tuple of three items.
# item #0 is the timestamp,
# item #1 is the event type,
# item #2 is the function name.
#
def getIntervalData(intervalBeginningsStack, intervalEnd, logfile):

    errorOccurred = False;
    matchFound = False;

    beginTimestamp = 0;
    beginFunctionName = "";

    endTimestamp = intervalEnd[0];
    eventType = intervalEnd[1];
    endFunctionName = intervalEnd[2];

    if (eventType != 1):
        logfile.write(
            "getIntervaldata: only rows with event type 1 can be used.\n");
        logfile.write(str(intervalEnd) + "\n");
        return None;

    if (len(intervalBeginningsStack) < 1):
        logfile.write("Nothing on the intervalBeginningsStack. " +
                      "I cannot find the beginning for this interval.\n");
        logfile.write(str(intervalEnd) + "\n");
        return None;

    while (not matchFound):
        intervalBegin = intervalBeginningsStack.pop();
        if (intervalBegin is None):
            logfile.write("Could not find the matching operation begin record" +
                          " for the following operation end record: \n");
            logfile.write(str(intervalEnd) + "\n");
            return None;

        beginTimestamp = intervalBegin[0];
        beginFunctionName = intervalBegin[2];
        if (beginFunctionName != endFunctionName):
            logfile.write("Operation end record does not match the available " +
                          "operation begin record. " +
                          "Your log file may be incomplete.\n" +
                          "Skipping the begin record.\n");
            logfile.write("Begin: " + str(intervalBegin) + "\n");
            logfile.write("End: " + str(intervalEnd) + "\n");
            errorOccurred = True;
        else:
            matchFound = True;

    return beginTimestamp, endTimestamp, endFunctionName, errorOccurred;

def createCallstackSeries(data, logfilename):

    beginIntervals = [];
    dataFrame = None;
    endIntervals = [];
    errorReported = False;
    functionNames = [];
    intervalBeginningsStack = [];
    largestStackDepth = 0;
    logfile = None;
    thisIsFirstRow = True;

    # Let's open the log file.
    try:
        logfile = open(logfilename, "w");
    except:
        logfile = sys.stdout;

    for row in data.itertuples():

        timestamp = row[0];
        eventType = row[1];
        function = row[2];

        if (eventType == 0):
            intervalBeginningsStack.append(row);
        elif (eventType == 1):
            try:
                intervalBegin, intervalEnd, function, error\
                    = getIntervalData(intervalBeginningsStack, row, logfile);
                if (error and (not errorReported)):
                    errorReported = reportDataError(logfile, logfilename);
            except:
                if (not errorReported):
                    errorReported = reportDataError(logfile, logfilename);
                continue;

            beginIntervals.append(intervalBegin);
            endIntervals.append(intervalEnd);
            functionNames.append(function);

        else:
            print("Invalid event in this line:");
            print(str(timestamp) + " " + str(eventType) + " " + str(function));
            continue;

    if (len(intervalBeginningsStack) > 0):
        logfile.write(str(len(intervalBeginningsStack)) + " operations had a " +
                      "begin record, but no matching end records. " +
                      "Please check that your operation tracking macros " +
                      "are properly inserted.\n");
        if (not errorReported):
            errorReported = reportDataError(logfile, logfilename);
        intervalBeginningsStack = [];

    dataDict = {};
    dataDict['start'] = beginIntervals;
    dataDict['end'] = endIntervals;
    dataDict['function'] = functionNames;
    dataDict['stackdepth'] = [0] * len(beginIntervals);

    dataframe = pd.DataFrame(data=dataDict);
    dataframe = assignStackDepths(dataframe);

    dataframe['durations'] = dataframe['end'] - dataframe['start'];
    dataframe['stackdepthNext'] = dataframe['stackdepth'] + 1;

    return dataframe;

def checkForTimestampAndGetRowSkip(fname):

    firstTimeStamp = 0;

    with open(fname) as f:
        firstLine = f.readline();

        firstLine = firstLine.strip();
        words = firstLine.split(" ");

        if (len(words) == 1):
            try:
                firstTimeStamp = int(words[0]);
            except ValueError:
                print(color.BOLD + color.RED +
                      "Could not parse seconds since Epoch on first line" +
                      color.END);
                firstTimeStamp = 0;
            return firstTimeStamp, 1;
        else:
            return firstTimeStamp, 0;

#
# Find the session ID in the file name. The format of the input file name is
# optrack.<PID>.<session-id>-<internal/external>.txt
#
def getSessionFromFileName(fname):

    words = fname.split(".");

    if (len(words) < 4):
        return 0;

    words = words[2].split("-");
    if (len(words) > 1):
        try:
            sid = int(words[0]);
            return sid;
        except:
            return 0;
    else:
        return 0;

def makeCSVFname(fname):

    words = fname.split(".");

    if (len(words) > 0):
        words[len(words)-1] = "csv";

    return ".".join(words);
#
# The input is the dataframe, where each record has a function name, its
# begin timestamp, its end timestamp and its stackdepth. This funciton will
# aggregate this data to determine the percentage of time we spent in each
# function in each interval.
#
def parseIntervals(df, firstTimeStamp, fname):

    global intervalLength;
    global unitsPerSecond;

    # The output dataframe has a time column and then a column for
    # each unique function in this file. Then there is one row
    # per interval.
    #
    outputDict = {};
    outputDict['time'] = [];

    sessionID = getSessionFromFileName(fname);
    columnNamePrefix = "#units=%;section=Session " + str(sessionID) + ";name=";

    # Get a list of all functions that we have in the input data frame.
    # Each function will be a column in the output dataframe.

    allFuncs = df['function'].unique();
    for i in range (0, len(allFuncs)):
        outputDict[columnNamePrefix + allFuncs[i]] = [];

    # We have two time formats. The data in the file is using fine-granular
    # time units, mostly likely from the CPU's cycle counter. The output
    # format will use coarse-granular time intervals in seconds. So we need
    # to convert the units of the input data to seconds.
    #
    firstTimestampUnits = df['start'].iloc[0];
    lastTimestampUnits = df['end'].iloc[-1];

    firstIntervalTimestampSeconds = firstTimeStamp;
    lastIntervalTimestampSeconds = firstIntervalTimestampSeconds + \
                (lastTimestampUnits - firstTimestampUnits) \
                // unitsPerSecond;

    if (lastIntervalTimestampSeconds < firstIntervalTimestampSeconds):
        print(color.BOLD + color.RED +
              "The first timestamp in seconds is " +
              str(firstIntervalTimestampSeconds) + ", but the last one " +
              "appears to be smaller: " + str(lastIntervalTimestampSeconds) +
              ". Skipping this file." + color.END);
        return;

    currentIntervalSeconds = firstIntervalTimestampSeconds;
    currentIntBeginUnits = firstTimestampUnits;

    # For each function in the current interval compute the aggregate
    # duration that it executed in the current interval.
    while currentIntervalSeconds <= lastIntervalTimestampSeconds:

        thisIntDict = {};

        outputDict['time'].append(currentIntervalSeconds);

        currentIntEndUnits = currentIntBeginUnits + \
                             intervalLength * unitsPerSecond;

        # Select all functions, whose begin and end time fall within the
        # current interval.
        # Entire function duration gets added for functions that begin and
        # end during this interval.

        beginAndEndInInterval = df.loc[(df['start'] >= currentIntBeginUnits)
                                       & (df['start'] <= currentIntEndUnits)
                                       & (df['end'] >= currentIntBeginUnits)
                                       & (df['end'] <= currentIntEndUnits)];

        for index, row in beginAndEndInInterval.iterrows():
            func = row['function'];
            duration = row['end'] - row['start'];
            if (func not in thisIntDict):
                thisIntDict[func] = duration;
            else:
                thisIntDict[func] += duration;

        # Select all functions, whose begin timestamp is within this
        # interval, but the end timestamp is outside of it.
        # Only the duration up to the end of the interval gets added
        # for functions that begin during this interval, but end
        # outside of it.

        beginInInterval = df.loc[(df['start'] >= currentIntBeginUnits)
                                 & (df['start'] <= currentIntEndUnits)
                                 & (df['end'] > currentIntEndUnits)];

        for index, row in beginInInterval.iterrows():
            func = row['function'];
            duration = currentIntEndUnits - row['start'];
            if (func not in thisIntDict):
                thisIntDict[func] = duration;
            else:
                thisIntDict[func] += duration;

        # Select all functions, whose end timestamp is within this
        # interval, but the begin timestamp is in an earlier interval.
        # For functions that end in the interval, but begin outside it
        # we add the portion of the duration from the beginning of the
        # interval and until the function end time.

        endInInterval = df.loc[(df['start'] < currentIntBeginUnits)
                               & (df['end'] >= currentIntBeginUnits)
                               & (df['end'] <= currentIntEndUnits)];

        for index, row in endInInterval.iterrows():
            func = row['function'];
            duration = row['end'] - currentIntBeginUnits;
            if (func not in thisIntDict):
                thisIntDict[func] = duration;
            else:
                thisIntDict[func] += duration;

        # Select all functions, whose begin timestamp is in an earlier
        # interval and end timestamp is in a later interval.
        # For functions that last during the entire interval the duration
        # equivalent to the interval's length gets added.

        beginEndOutsideInterval = df.loc[(df['start'] < currentIntBeginUnits)
                                &  (df['end'] > currentIntEndUnits)];

        for index, row in beginEndOutsideInterval.iterrows():
            func = row['function'];
            duration = intervalLength * unitsPerSecond;
            if (func not in thisIntDict):
                thisIntDict[func] = duration;
            else:
                thisIntDict[func] += duration;

        # Convert the durations to percentages and record them
        # in the output dictionary
        for func, duration in thisIntDict.items():
            outputDictKey =  columnNamePrefix + func;
            percentDuration = float(duration) // \
                              float(intervalLength * unitsPerSecond) * 100;
            outputDict[outputDictKey].append(percentDuration);

        # In the output dictionary find all functions that did not
        # execute during this interval and append zero.
        # The list at each function's key should be as long as the list
        # at key 'time'.
        targetLen = len(outputDict['time']);
        for key, theList in outputDict.items():
            if len(theList) < targetLen:
                theList.append(0);

        currentIntervalSeconds += intervalLength;
        currentIntBeginUnits = currentIntEndUnits + 1;

    # Make the dataframe from the dictionary. Arrange the columns
    # such that 'time' is first.
    #
    targetColumns = ['time'];

    for key, value in outputDict.items():
        if key != 'time':
            targetColumns.append(key);

    outputDF = pd.DataFrame(data=outputDict, columns = targetColumns);

    # Write the data to file
    outputCSV = makeCSVFname(fname);
    outputDF.to_csv(path_or_buf=outputCSV, index=False, header=True);


def processFile(fname):

    firstTimeStamp, skipRows = checkForTimestampAndGetRowSkip(fname);

    rawData = pd.read_csv(fname,
                          header=None, delimiter=" ",
                          index_col=2,
                          names=["Event", "Function", "Timestamp"],
                          dtype={"Event": np.int32, "Timestamp": np.int64},
                          thousands=",", skiprows = skipRows);

    print(color.BOLD + color.BLUE +
          "Processing file " + str(fname) + color.END);

    iDF = createCallstackSeries(rawData, "." + fname + ".log");

    if not iDF.empty:
        parseIntervals(iDF, firstTimeStamp, fname);

def waitOnOneProcess(runningProcesses):

    i = 0;
    success = False;
    while i < len(runningProcesses):
        p = runningProcesses[i];
        if (not p.is_alive()):
            del runningProcesses[i];
            success = True;
        else:
            i+=1;

    # If we have not found a terminated process, sleep for a while
    if (not success):
        time.sleep(1);

def main():

    runnableProcesses = [];
    runningProcesses = [];

    # Set up the argument parser
    #
    parser = argparse.ArgumentParser(description=
                                 'Convert operation tracking log files \
                                 to the csv for visualization with t2.');
    parser.add_argument('files', type=str, nargs='*',
                        help='log files to process');
    parser.add_argument('-j', dest='jobParallelism', type=int,
                        default='0');

    args = parser.parse_args();

    if (len(args.files) == 0):
        parser.print_help();
        sys.exit(1);

    # Determine the target job parallelism
    if (args.jobParallelism > 0):
        targetParallelism = args.jobParallelism;
    else:
        targetParallelism = multiprocessing.cpu_count() * 2;

    # Process all files in parallel
    for fname in args.files:
        p = Process(target=processFile,
                    args=(fname,));
        runnableProcesses.append(p);

    while (len(runnableProcesses) > 0):
        while (len(runningProcesses) < targetParallelism
               and len(runnableProcesses) > 0):

            p = runnableProcesses.pop();
            p.start();
            runningProcesses.append(p);

        # Find at least one terminated process
        waitOnOneProcess(runningProcesses);

    # Wait for all processes to terminate
    while (len(runningProcesses) > 0):
        waitOnOneProcess(runningProcesses);

if __name__ == '__main__':
    main()
