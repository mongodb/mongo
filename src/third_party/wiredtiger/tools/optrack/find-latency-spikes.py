#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
from bokeh.layouts import column
from bokeh.models import ColumnDataSource, CustomJS, HoverTool, FixedTicker
from bokeh.models import  LabelSet, Legend, LegendItem
from bokeh.models import NumeralTickFormatter, OpenURL, Range1d, TapTool
from bokeh.models.annotations import Label
from bokeh.plotting import figure, output_file, reset_output, save, show
from bokeh.resources import CDN
import matplotlib
import numpy as np
import os
import pandas as pd
import sys
import traceback

# A directory where we store cross-file plots for each bucket of the outlier
# histogram.
#
bucketDir = "BUCKET-FILES";

# A static list of available CSS colors
colorList = [];

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

# A function name mapped to its corresponding color.
#
funcToColor = {};
lastColorUsed = 0;

# The smallest and the largest timestamps seen across all files.
#
firstTimeStamp = sys.maxsize;
lastTimeStamp = 0;

# A dictionary that holds function-specific threshold values telling
# us when the function is to be considered an outlier. These values
# would be read from a config file, if supplied by the user.
#
outlierThresholdDict = {};
outlierPrettyNames = {};

# A dictionary that holds a reference to the raw dataframe for each file.
#
perFileDataFrame = {};

# A dictionary that holds the intervals data per function.
#
perFuncDF = {};

# Data frames and largest stack depth for each file.
perFileDataFrame = {};
perFileLargestStackDepth = {};

plotWidth = 1200;
pixelsForTitle = 30;
pixelsPerHeightUnit = 30;
pixelsPerWidthUnit = 5;

# The name of the time units that were used when recording timestamps.
# We assume that it's nanoseconds by default. Alternative units can be
# set in the configuration file.
#
timeUnitString = "nanoseconds";

# The coefficient by which we multiply the standard deviation when
# setting the outlier threshold, in case it is not specified by the user.
#
STDEV_MULT = 2;

def initColorList():

    global colorList;

    colorList = matplotlib.colors.cnames.keys();

    for color in colorList:
        # Some browsers break if you try to give them 'sage'
        if (color == "sage"):
            colorList.remove(color);

#
# Each unique function name gets a unique color.
# If we run out of colors, we repeat them from the
# beginning of the list.
#
def getColorForFunction(function):

    global colorList;
    global lastColorUsed;
    global funcToColor;

    if not funcToColor.has_key(function):
        funcToColor[function] = colorList[lastColorUsed % len(colorList)];
        lastColorUsed += 1;

    return funcToColor[function];

#
# An intervalEnd is a tuple of three items.
# item #0 is the timestamp,
# item #1 is the event type,
# item #2 is the function name.
#
def getIntervalData(intervalBeginningsStack, intervalEnd, logfile):

    errorOccurred = False;
    matchFound = False;

    if (intervalEnd[1] != 1):
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
        if (intervalBegin[2] != intervalEnd[2]):
            logfile.write("Operation end record does not match the available " +
                          "operation begin record. " +
                          "Your log file may be incomplete.\n" +
                          "Skipping the begin record.\n");
            logfile.write("Begin: " + str(intervalBegin) + "\n");
            logfile.write("End: " + str(intervalEnd) + "\n");
            errorOccurred = True;
        else:
            matchFound = True;

    return intervalBegin[0], intervalEnd[0], intervalEnd[2], errorOccurred;

def plotOutlierHistogram(dataframe, maxOutliers, func, durationThreshold,
                         averageDuration, maxDuration):

    global pixelsForTitle;
    global pixelsPerHeightUnit;
    global plotWidth;
    global timeUnitString;

    cds = ColumnDataSource(dataframe);

    figureTitle = "Occurrences of " + func + " that took longer than " \
                  + durationThreshold + ".";

    hover = HoverTool(tooltips = [
        ("interval start", "@lowerbound{0,0}"),
        ("interval end", "@upperbound{0,0}")]);

    TOOLS = [hover, "tap, reset"];

    p = figure(title = figureTitle, plot_width = plotWidth,
               plot_height = min(500, (max(5, (maxOutliers + 1)) \
                                       * pixelsPerHeightUnit + \
                                       pixelsForTitle)),
               x_axis_label = "Execution timeline (" + timeUnitString + ")",
               y_axis_label = "Number of outliers",
               tools = TOOLS, toolbar_location="above");

    y_ticker_max = p.plot_height / pixelsPerHeightUnit;
    y_ticker_step = max(1, (maxOutliers + 1)/y_ticker_max);
    y_upper_bound = (maxOutliers / y_ticker_step + 1) * y_ticker_step;

    p.yaxis.ticker = FixedTicker(ticks =
                                 range(0, y_upper_bound, y_ticker_step));
    p.ygrid.ticker = FixedTicker(ticks =
                                 range(0, y_upper_bound, y_ticker_step));
    p.xaxis.formatter = NumeralTickFormatter(format="0,");

    p.y_range = Range1d(0, y_upper_bound);

    p.quad(left = 'lowerbound', right = 'upperbound', bottom = 'bottom',
           top = 'height', color = funcToColor[func], source = cds,
           nonselection_fill_color=funcToColor[func],
           nonselection_fill_alpha = 1.0,
           line_color = "lightgrey",
           selection_fill_color = funcToColor[func],
           selection_line_color="grey"
    );

    # Add an annotation to the chart
    #
    y_max = dataframe['height'].max();
    text = "Average duration: " + '{0:,.0f}'.format(averageDuration) + \
           ". Maximum duration: " + '{0:,.0f}'.format(maxDuration) + ".";
    mytext = Label(x=0, y=y_upper_bound-y_ticker_step, text=text,
                   text_color = "grey", text_font = "helvetica",
                   text_font_size = "10pt",
                   text_font_style = "italic");
    p.add_layout(mytext);

    url = "@bucketfiles";
    taptool = p.select(type=TapTool);
    taptool.callback = OpenURL(url=url);

    return p;

# From all timestamps subtract the smallest observed timestamp, so that
# our execution timeline begins at zero.
# Cleanup the data to remove incomplete records and fix their effects.
#
def normalizeIntervalData():

    global firstTimeStamp;
    global perFileDataFrame;

    print(color.BLUE + color.BOLD + "Normalizing data..." + color.END);

    for file, df in perFileDataFrame.iteritems():
        df['origstart'] = df['start'];
        df['start'] = df['start'] - firstTimeStamp;
        df['end'] = df['end'] - firstTimeStamp;

def reportDataError(logfile, logfilename):

    if (logfile is not sys.stdout):
        print(color.BOLD + color.RED + "Your data may have errors. " +
              "Check the file " + logfilename + " for details." + color.END);
    return True;

#
# Go over all operation records in the dataframe and assign stack depths.
#
def assignStackDepths(dataframe):

    stack = [];

    df = dataframe.sort_values(by=['start']);
    df = df.reset_index(drop = True);

    for i in range(len(df.index)):

        myStartTime = df.at[i, 'start'];

        # Pop all items off stack whose end time is earlier than my
        # start time. They are not part of my stack, so I don't want to
        # count them.
        #
        while (len(stack) > 0 and stack[-1] < myStartTime):
            stack.pop();

        df.at[i, 'stackdepth'] = len(stack);
        stack.append(df.at[i, 'end']);

    return df;

def createCallstackSeries(data, logfilename):

    global firstTimeStamp;
    global lastTimeStamp;

    colors = [];
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
        # row[0] is the timestamp, row[1] is the event type,
        # row[2] is the function name.
        #
        if (row[1] == 0):
            intervalBeginningsStack.append(row);
        elif (row[1] == 1):
            try:
                intervalBegin, intervalEnd, function, error\
                    = getIntervalData(intervalBeginningsStack, row, logfile);
                if (error and (not errorReported)):
                    errorReported = reportDataError(logfile, logfilename);
            except:
                if (not errorReported):
                    errorReported = reportDataError(logfile, logfilename);
                continue;

            if (intervalBegin < firstTimeStamp):
                firstTimeStamp =  intervalBegin;
            if (intervalEnd > lastTimeStamp):
                lastTimeStamp = intervalEnd;

            colors.append(getColorForFunction(function));
            beginIntervals.append(intervalBegin);
            endIntervals.append(intervalEnd);
            functionNames.append(function);

        else:
            print("Invalid event in this line:");
            print(str(row[0]) + " " + str(row[1]) + " " + str(row[2]));
            continue;

    if (len(intervalBeginningsStack) > 0):
        logfile.write(str(len(intervalBeginningsStack)) + " operations had a " +
                      "begin record, but no matching end records. " +
                      "Please check that your operation tracking macros " +
                      "are properly inserted.\n");
        if (not errorReported):
            errorReported = reportDataError(logfile, logfilename);
        intervalBeginningsStack = [];

    dict = {};
    dict['color'] = colors;
    dict['start'] = beginIntervals;
    dict['end'] = endIntervals;
    dict['function'] = functionNames;
    dict['stackdepth'] = [0] * len(beginIntervals);

    dataframe = pd.DataFrame(data=dict);
    dataframe = assignStackDepths(dataframe);

    dataframe['durations'] = dataframe['end'] - dataframe['start'];
    dataframe['stackdepthNext'] = dataframe['stackdepth'] + 1;

    return dataframe;

# For each function we only show the legend once. In this dictionary we
# keep track of colors already used.
#
colorAlreadyUsedInLegend = {};

def createLegendFigure(legendDict):

    global pixelsForTitle;
    global plotWidth;

    FUNCS_PER_ROW = 5;
    HSPACE_BETWEEN_FUNCS = 10;
    VSPACE_BETWEEN_FUNCS = 1;

    funcs = [];
    colors = [];
    x_coords = [];
    y_coords = [];
    pixelsForLegendItem = 20;

    # Get a sorted list of functions and their
    # corresponding colors.
    #
    for func in sorted(legendDict.keys()):
        funcs.append(func);
        colors.append(legendDict[func]);

    # Figure out the coordinates of functions on the plot
    #
    for i in range(len(funcs)):

        x_coord = i % (FUNCS_PER_ROW) + 1;
        x_coord += i % (FUNCS_PER_ROW) *  HSPACE_BETWEEN_FUNCS;
        x_coords.append(x_coord);

        y_coord = (i/FUNCS_PER_ROW) + 1;
        y_coord += (i/FUNCS_PER_ROW) *  VSPACE_BETWEEN_FUNCS;
        y_coords.append(y_coord);

    data = {};
    data['func'] = funcs;
    data['color'] = colors;
    data['left'] = x_coords;
    data['bottom'] = y_coords;

    df = pd.DataFrame(data=data);

    max_ycoord = df['bottom'].max();
    df['bottom'] = (max_ycoord + 1) - df['bottom'];

    df['right'] = df['left'] + 1;
    df['top'] = df['bottom'] + 1;

    cds = ColumnDataSource(df);

    p = figure(title="TRACKED FUNCTIONS",
               plot_width=plotWidth,
               plot_height = max((max_ycoord + 2) * pixelsForLegendItem, 90),
               tools = [], toolbar_location="above",
               x_range = (0, (FUNCS_PER_ROW + 1)* HSPACE_BETWEEN_FUNCS),
               y_range = (0, max_ycoord + 2),
               x_axis_label = "",
               y_axis_label = "");

    p.title.align = "center";
    p.title.text_font_style = "normal";

    p.xaxis.axis_line_color = "lightgrey";
    p.xaxis.major_tick_line_color = None;
    p.xaxis.minor_tick_line_color = None;
    p.xaxis.major_label_text_font_size = '0pt';

    p.yaxis.axis_line_color = "lightgrey";
    p.yaxis.major_tick_line_color = None;
    p.yaxis.minor_tick_line_color = None;
    p.yaxis.major_label_text_font_size = '0pt';

    p.xgrid.grid_line_color = None;
    p.ygrid.grid_line_color = None;

    p.outline_line_width = 1
    p.outline_line_alpha = 1
    p.outline_line_color = "lightgrey"

    p.quad(left = 'left', right = 'right', bottom = 'bottom',
           top = 'top', color = 'color', line_color = "lightgrey",
           line_width = 0.5, source=cds);

    labels = LabelSet(x='right', y='bottom', text='func', level='glyph',
                      text_font_size = "10pt",
                      x_offset=3, y_offset=0, source=cds, render_mode='canvas');
    p.add_layout(labels);

    return p;

def generateBucketChartForFile(figureName, dataframe, y_max, x_min, x_max):

    global colorAlreadyUsedInLegend;
    global funcToColor;
    global plotWidth;
    global timeUnitString;

    MAX_ITEMS_PER_LEGEND = 10;
    numLegends = 0;
    legendItems = {};
    pixelsPerStackLevel = 30;
    pixelsPerLegend = 60;
    pixelsForTitle = 30;

    cds = ColumnDataSource(dataframe);

    hover = HoverTool(tooltips=[
        ("function", "@function"),
        ("duration", "@durations{0,0}"),
        ("log file begin timestamp", "@origstart{0,0}")
    ]);

    TOOLS = [hover];

    p = figure(title=figureName, plot_width=plotWidth,
               x_range = (x_min, x_max),
               y_range = (0, y_max+1),
               x_axis_label = "Time (" + timeUnitString + ")",
               y_axis_label = "Stack depth",
               tools = TOOLS, toolbar_location="above");

    # No minor ticks or labels on the y-axis
    p.yaxis.major_tick_line_color = None;
    p.yaxis.minor_tick_line_color = None;
    p.yaxis.major_label_text_font_size = '0pt';
    p.yaxis.ticker = FixedTicker(ticks = range(0, y_max+1));
    p.ygrid.ticker = FixedTicker(ticks = range(0, y_max+1));

    p.xaxis.formatter = NumeralTickFormatter(format="0,");
    p.title.text_font_style = "bold";

    p.quad(left = 'start', right = 'end', bottom = 'stackdepth',
           top = 'stackdepthNext', color = 'color', line_color = "lightgrey",
           line_width = 0.5, source=cds);

    for func, fColor in funcToColor.iteritems():

        # If this function is not present in this dataframe,
        # we don't care about it.
        #
        boolVec = (dataframe['function'] == func);
        fDF = dataframe[boolVec];
        if (fDF.size == 0):
            continue;

        # If we already added a color to any legend, we don't
        # add it again to avoid redundancy in the charts and
        # in order not to waste space.
        #
        if (colorAlreadyUsedInLegend.has_key(fColor)):
            continue;
        else:
            colorAlreadyUsedInLegend[fColor] = True;

        legendItems[func] = fColor;

    # Plot height is the function of the maximum call stack and the number of
    # legends
    p.plot_height =  max((y_max+1) * pixelsPerStackLevel, 100) + pixelsForTitle;

    return p, legendItems;

def generateEmptyDataset():

    dict = {};
    dict['color'] = [0];
    dict['durations'] = [0];
    dict['start'] = [0];
    dict['end'] = [0];
    dict['function'] = [""];
    dict['stackdepth'] = [0];
    dict['stackdepthNext'] = [0];

    return pd.DataFrame(data=dict);

#
# Here we generate plots that span all the input files. Each plot shows
# the timelines for all files, stacked vertically. The timeline shows
# the function callstacks over time from this file.
#
# Since a single timeline is too large to fit on a single screen, we generate
# a separate HTML file with plots for bucket "i". A bucket is a vertical slice
# across the timelines for all files. We call it a bucket, because it
# corresponds to a bucket in the outlier histogram.
#
def generateCrossFilePlotsForBucket(i, lowerBound, upperBound, navigatorDF):

    global bucketDir;
    global colorAlreadyUsedInLegend;
    global timeUnitString;

    aggregateLegendDict = {};
    figuresForAllFiles = [];
    fileName = bucketDir + "/bucket-" + str(i) + ".html";

    reset_output();

    intervalTitle = "Interval #" + str(i) + ". {:,}".format(lowerBound) + \
                    " to " + "{:,}".format(upperBound) + \
                    " " + timeUnitString + ".";

    # Generate a navigator chart, which shows where we are in the
    # trace and allows moving around the trace.
    #
    navigatorFigure = generateNavigatorFigure(navigatorDF, i, intervalTitle);
    figuresForAllFiles.append(navigatorFigure);

    # The following dictionary keeps track of legends. We need
    # a legend for each new HTML file. So we reset the dictionary
    # before generating a new file.
    #
    colorAlreadyUsedInLegend = {};

    # Select from the dataframe for this file the records whose 'start'
    # and 'end' timestamps fall within the lower and upper bound.
    #
    for fname in sorted(perFileDataFrame.keys()):

        fileDF = perFileDataFrame[fname];

        # Select operations whose start timestamp falls within
        # the current interval, delimited by lowerBound and upperBound.
        #
        startInBucket = fileDF.loc[(fileDF['start'] >= lowerBound)
                                   & (fileDF['start'] < upperBound)];

        # Select operations whose end timestamp falls within
        # the current interval, delimited by lowerBound and upperBound.
        #
        endInBucket = fileDF.loc[(fileDF['end'] > lowerBound)
                                   & (fileDF['end'] <= upperBound)];

        # Select operations that begin before this interval and end after
        # this interval, but continue throughout this interval. The interval
        # is delimited by lowerBound and upperBound.
        #
        spanBucket = fileDF.loc[(fileDF['start'] < lowerBound)
                                   & (fileDF['end'] > upperBound)];

        frames = [startInBucket, endInBucket, spanBucket];
        bucketDF = pd.concat(frames).drop_duplicates().reset_index(drop=True);

        if (bucketDF.size == 0):
            continue;

        # If the end of the function is outside the interval, let's pretend
        # that it is within the interval, otherwise we won't see any data about
        # it when we hover. This won't have the effect of showing wrong
        # data to the user.
        #
        mask = bucketDF.end >= upperBound;
        bucketDF.loc[mask, 'end'] = upperBound-1;

        # Same adjustment as above if the start of the operation falls outside
        # the interval's lower bound.
        #
        mask = bucketDF.start < lowerBound;
        bucketDF.loc[mask, 'start'] = lowerBound;

        largestStackDepth = bucketDF['stackdepthNext'].max();
        figureTitle = fname;

        figure, legendDict = generateBucketChartForFile(figureTitle, bucketDF,
                                                        largestStackDepth,
                                                        lowerBound, upperBound);
        aggregateLegendDict.update(legendDict);
        figuresForAllFiles.append(figure);

    # Create the legend for this file and insert it after the navigator figure
    if (len(aggregateLegendDict) > 0):
        legendFigure = createLegendFigure(aggregateLegendDict);
        figuresForAllFiles.insert(1, legendFigure);

    save(column(figuresForAllFiles), filename = fileName,
         title=intervalTitle, resources=CDN);

    return fileName;

# Generate a plot that shows a view of the entire timeline in a form of
# intervals. By clicking on an interval we can navigate to that interval.
#
def generateNavigatorFigure(dataframe, i, title):

    global pixelsForTitle;
    global pixelsPerHeightUnit;
    global plotWidth;

    # Generate the colors, such that the current interval is shown in a
    # different color than the rest.
    #
    numIntervals = dataframe['intervalnumber'].size;
    color = ["white" for x in range(numIntervals)];
    color[i] = "salmon";
    dataframe['color'] = color;

    cds = ColumnDataSource(dataframe);

    title = title + " CLICK TO NAVIGATE";

    hover = HoverTool(tooltips = [
        ("interval #", "@intervalnumber"),
        ("interval start", "@intervalbegin{0,0}"),
        ("interval end", "@intervalend{0,0}")]);

    TOOLS = [hover, "tap"];

    p = figure(title = title, plot_width = plotWidth,
               x_range = (0, numIntervals),
               plot_height =  2 * pixelsPerHeightUnit + pixelsForTitle,
               x_axis_label = "",
               y_axis_label = "", tools = TOOLS,
               toolbar_location="above");

    # No minor ticks or labels on the y-axis
    p.yaxis.major_tick_line_color = None;
    p.yaxis.minor_tick_line_color = None;
    p.yaxis.major_label_text_font_size = '0pt';
    p.yaxis.ticker = FixedTicker(ticks = range(0, 1));
    p.ygrid.ticker = FixedTicker(ticks = range(0, 1));

    p.xaxis.formatter = NumeralTickFormatter(format="0,");

    p.title.align = "center";
    p.title.text_font_style = "normal";

    p.quad(left = 'intervalnumber', right = 'intervalnumbernext',
           bottom = 0, top = 2, color = 'color', source = cds,
           nonselection_fill_color='color',
           nonselection_fill_alpha = 1.0,
           line_color = "aliceblue",
           selection_fill_color = "white",
           selection_line_color="lightgrey"
    );

    url = "@bucketfiles";
    taptool = p.select(type=TapTool);
    taptool.callback = OpenURL(url=url);

    return p;


# Create a dataframe describing all time intervals, which will later be used
# to generate a plot allowing us to navigate along the execution by clicking
# on different intervals.
#
def createIntervalNavigatorDF(numBuckets, timeUnitsPerBucket):

    global bucketDir;

    bucketFiles = [];
    bucketID = [];
    intervalBegin = [];
    intervalEnd = [];

    for i in range(numBuckets):

        lBound = i * timeUnitsPerBucket;
        uBound = (i+1) * timeUnitsPerBucket;
        fileName = "bucket-" + str(i) + ".html";

        bucketID.append(i);
        intervalBegin.append(lBound);
        intervalEnd.append(uBound);
        bucketFiles.append(fileName);

    data = {};
    data['bucketfiles'] = bucketFiles;
    data['intervalbegin'] =  intervalBegin;
    data['intervalend'] =  intervalEnd;
    data['intervalnumber'] = bucketID;

    dataframe = pd.DataFrame(data=data);
    dataframe['intervalnumbernext'] = dataframe['intervalnumber'] + 1;
    return dataframe;

# Generate plots of time series slices across all files for each bucket
# in the outlier histogram. Save each cross-file slice to an HTML file.
#
def generateTSSlicesForBuckets():

    global firstTimeStamp;
    global lastTimeStamp;
    global plotWidth;
    global pixelsPerWidthUnit;

    bucketFilenames = [];

    numBuckets = plotWidth / pixelsPerWidthUnit;
    timeUnitsPerBucket = (lastTimeStamp - firstTimeStamp) / numBuckets;

    navigatorDF = createIntervalNavigatorDF(numBuckets, timeUnitsPerBucket);

    for i in range(numBuckets):
        lowerBound = i * timeUnitsPerBucket;
        upperBound = (i+1) * timeUnitsPerBucket;

        fileName = generateCrossFilePlotsForBucket(i, lowerBound, upperBound,
                                                   navigatorDF);

        percentComplete = float(i) / float(numBuckets) * 100;
        print(color.BLUE + color.BOLD + " Generating timeline charts... "),
        sys.stdout.write("%d%% complete  \r" % (percentComplete) );
        sys.stdout.flush();
        bucketFilenames.append(fileName);

    print(color.END);

    return bucketFilenames;

def processFile(fname):

    global perFileDataFrame;
    global perFuncDF;

    rawData = pd.read_csv(fname,
                       header=None, delimiter=" ",
                       index_col=2,
                       names=["Event", "Function", "Timestamp"],
                       dtype={"Event": np.int32, "Timestamp": np.int64},
                       thousands=",");

    print(color.BOLD + color.BLUE +
          "Processing file " + str(fname) + color.END);
    iDF = createCallstackSeries(rawData, "." + fname + ".log");

    perFileDataFrame[fname] = iDF;

    for func in funcToColor.keys():

        funcDF = iDF.loc[lambda iDF: iDF.function == func, :];
        funcDF = funcDF.drop(columns = ['function']);

        if (not perFuncDF.has_key(func)):
            perFuncDF[func] = funcDF;
        else:
            perFuncDF[func] = pd.concat([perFuncDF[func], funcDF]);


#
# For each function, split the timeline into buckets. In each bucket
# show how many times this function took an unusually long time to
# execute.
#
# The parameter durationThreshold tells us when a function should be
# considered as unusually long. If this parameter is "-1" we count
# all functions whose duration exceeded the average by more than
# two standard deviations.
#
def createOutlierHistogramForFunction(func, funcDF, bucketFilenames):

    global firstTimeStamp;
    global lastTimeStamp;
    global plotWidth;
    global pixelsPerWidthUnit;
    global timeUnitString;
    global STDEV_MULT;

    durationThreshold = 0;
    durationThresholdDescr = "";

    #
    # funcDF is a list of functions along with their start and end
    # interval and durations. We need to create a new dataframe where
    # we separate the entire timeline into a fixed number of periods
    # and for each period compute how many outlier durations were
    # observed. Then we create a histogram from this data.

    # Subtract the smallest timestamp from all the interval data.
    funcDF['start'] = funcDF['start'] - firstTimeStamp;
    funcDF['end'] = funcDF['end'] - firstTimeStamp;

    funcDF = funcDF.sort_values(by=['start']);

    averageDuration = funcDF['durations'].mean();
    maxDuration = funcDF['durations'].max();

    if (outlierThresholdDict.has_key(func)):
        durationThreshold = outlierThresholdDict[func];
        durationThresholdDescr = outlierPrettyNames[func];
    elif (outlierThresholdDict.has_key("*")):
        durationThreshold = outlierThresholdDict["*"];
        durationThresholdDescr = outlierPrettyNames["*"];
    else:
        # Signal that we will use standard deviation
        durationThreshold  = -STDEV_MULT;

    if (durationThreshold < 0): # this is a stdev multiplier
        mult = -durationThreshold;
        stdDev = funcDF['durations'].std();
        durationThreshold = averageDuration + mult * stdDev;
        durationThresholdDescr = '{0:,.0f}'.format(durationThreshold) \
                                 + " " + timeUnitString + " (" + str(mult) + \
                                 " standard deviations)";

    numBuckets = plotWidth / pixelsPerWidthUnit;
    timeUnitsPerBucket = (lastTimeStamp - firstTimeStamp) / numBuckets;
    lowerBounds = [];
    upperBounds = [];
    bucketHeights = [];
    maxOutliers = 0;

    for i in range(numBuckets):
        lowerBound = i * timeUnitsPerBucket;
        upperBound = (i+1) * timeUnitsPerBucket;

        bucketDF = funcDF.loc[(funcDF['start'] >= lowerBound)
                                & (funcDF['start'] < upperBound)
                                & (funcDF['durations'] >= durationThreshold)];

        numOutliers = bucketDF.size;
        if (numOutliers > maxOutliers):
            maxOutliers = numOutliers;

        lowerBounds.append(lowerBound);
        upperBounds.append(upperBound);
        bucketHeights.append(numOutliers);

    if (maxOutliers == 0):
        return None;

    dict = {};
    dict['lowerbound'] = lowerBounds;
    dict['upperbound'] = upperBounds;
    dict['height'] = bucketHeights;
    dict['bottom'] = [0] * len(lowerBounds);
    dict['bucketfiles'] = bucketFilenames;

    dataframe = pd.DataFrame(data=dict);

    return plotOutlierHistogram(dataframe, maxOutliers, func,
                                durationThresholdDescr, averageDuration,
                                maxDuration);

#
# Return the string naming the time units used to measure time stamps,
# depending on how many time units there are in a second.
#
def getTimeUnitString(unitsPerSecond):

    if unitsPerSecond == 1000:
        return "milliseconds";
    elif unitsPerSecond == 1000000:
        return "microseconds";
    elif unitsPerSecond == 1000000000:
        return "nanoseconds";
    else:
        return "CPU cycles";

#
# The configuration file tells us which functions should be considered
# outliers. All comment lines must begin with '#'.
#
# The first non-comment line of the file must tell us how to interpret
# the measurement units in the trace file. It must have a single number
# telling us how many time units are contained in a second. This should
# be the same time units used in the trace file. For example, if the trace
# file contains timestamps measured in milliseconds, the number would be 1000,
# it the timestamp is in nanoseconds, the number would be 1000000000.
# If timestamps were measured in clock cycles, the number
# must tell us how many times the CPU clock ticks per second on the processor
# where the trace was gathered.
#
# The remaining lines must have the format:
#       <func_name> <outlier_threshold> [units]
#
# For example, if you would like to flag as outliers all instances of
# __cursor_row_search that took longer than 200ms, you would specify this as:
#
#        __cursor_row_search 200 ms
#
# You can use * as the wildcard for all function. No other wildcard options are
# supported at the moment.
#
# Acceptable units are:
#
# s -- for seconds
# ms -- for milliseconds
# us -- for microseconds
# ns -- for nanoseconds
# stdev -- for standard deviations.
#
# If no units are supplied, the same unit as the one used for the timestamp
# in the trace files is assumed.
#
# If there is a valid configuration file, but the function does not appear in
# it, we will not generate an outlier histogram for this function. Use the
# wildcard symbol to include all functions.
#
def parseConfigFile(fname):

    global outlierThresholdDict;
    global outlierPrettyNames;
    global timeUnitString;

    configFile = None;
    firstNonCommentLine = True;
    unitsPerSecond = -1;
    unitsPerMillisecond = 0.0;
    unitsPerMicrosecond = 0.0;
    unitsPerNanosecond = 0.0;

    try:
        configFile = open(fname, "r");
    except:
        print(color.BOLD + color.RED +
              "Could not open " + fname + " for reading." + color.END);
        return False;

    for line in configFile:

        if (line[0] == "#"):
            continue;
        elif (firstNonCommentLine):
            try:
                unitsPerSecond = int(line);
                unitsPerMillisecond = unitsPerSecond / 1000;
                unitsPerMicrosecond = unitsPerSecond / 1000000;
                unitsPerNanosecond  = unitsPerSecond / 1000000000;

                timeUnitString = getTimeUnitString(unitsPerSecond);

                firstNonCommentLine = False;
            except ValueError:
                print(color.BOLD + color.RED +
                      "Could not parse the number of measurement units " +
                      "per second. This must be the first value in the " +
                      "config file." + color.END);
                return False;
        else:
            func = "";
            number = 0;
            threshold = 0.0;
            units = "";

            words = line.split();
            try:
                func = words[0];
                number = int(words[1]);
                units = words[2];
            except ValueError:
                print(color.BOLD + color.RED +
                      "While parsing the config file, could not understand " +
                      "the following line: " + color.END);
                print(line);
                continue;

            # Now convert the number to the baseline units and record in the
            # dictionary.
            #
            if (units == "s"):
                threshold = unitsPerSecond * number;
            elif (units == "ms"):
                threshold = unitsPerMillisecond * number;
            elif (units == "us"):
                threshold = unitsPerMicrosecond * number;
            elif (units == "ns"):
                threshold = unitsPerNanosecond * number;
            elif (units == "stdev"):
                threshold = -units;
                # We record it as negative, so that we know
                # this is a standard deviation. We will compute
                # the actual value once we know the average.
            else:
                print(color.BOLD + color.RED +
                      "While parsing the config file, could not understand " +
                      "the following line: " + color.END);
                print(line);
                continue;

            outlierThresholdDict[func] = threshold;
            outlierPrettyNames[func] = str(number) + " " + units;

    # We were given an empty config file
    if (firstNonCommentLine):
        return False;

    print outlierThresholdDict;
    return True;


def main():

    global arrowLeftImg;
    global arrowRightImg;
    global bucketDir;
    global perFuncDF;

    configSupplied = False;
    figuresForAllFunctions = [];

    # Set up the argument parser
    #
    parser = argparse.ArgumentParser(description=
                                 'Visualize operation log');
    parser.add_argument('files', type=str, nargs='*',
                        help='log files to process');
    parser.add_argument('-c', '--config', dest='configFile', default='');
    args = parser.parse_args();

    if (len(args.files) == 0):
        parser.print_help();
        sys.exit(1);

    # Get names of standard CSS colors that we will use for the legend
    initColorList();

    # Read the configuration file, if supplied.
    if (args.configFile != ''):
        configSupplied = parseConfigFile(args.configFile);

    if (not configSupplied):
        pluralSuffix = "";
        if (STDEV_MULT > 1):
            pluralSuffix = "s";
        print(color.BLUE + color.BOLD +
              "Will deem as outliers all function instances whose runtime " +
              "was " + str(STDEV_MULT) + " standard deviation" + pluralSuffix +
              " greater than the average runtime for that function."
              + color.END);


    # Create a directory for the files that display the data summarized
    # in each bucket of the outlier histogram. We call these "bucket files".
    #
    if not os.path.exists(bucketDir):
        os.makedirs(bucketDir);

    # Parallelize this later, so we are working on files in parallel.
    for fname in args.files:
        processFile(fname);

    # Normalize all intervals by subtracting the first timestamp.
    normalizeIntervalData();

    # Generate plots of time series slices across all files for each bucket
    # in the outlier histogram. Save each cross-file slice to an HTML file.
    #
    fileNameList = generateTSSlicesForBuckets();

    totalFuncs = len(perFuncDF.keys());
    i = 0;
    # Generate a histogram of outlier durations
    for func in sorted(perFuncDF.keys()):
        funcDF = perFuncDF[func];
        figure = createOutlierHistogramForFunction(func, funcDF, fileNameList);
        if (figure is not None):
            figuresForAllFunctions.append(figure);

        i += 1;
        percentComplete = float(i) / float(totalFuncs) * 100;
        print(color.BLUE + color.BOLD + " Generating outlier histograms... "),
        sys.stdout.write("%d%% complete  \r" % (percentComplete) );
        sys.stdout.flush();

    print(color.END);
    reset_output();
    output_file(filename = "WT-outliers.html", title="Outlier histograms");
    show(column(figuresForAllFunctions));

if __name__ == '__main__':
    main()
