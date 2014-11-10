#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from optparse import OptionParser
from string import Template
import json

template_content_nvd3 = """
$container
$jschart
"""

template_page_nvd3 = """
<!DOCTYPE html>
<html lang="en">
<head>
$header
</head>
<body>
%s
</body>
""" % template_content_nvd3


def stab(tab=1):
    """
    create space tabulation
    """
    return ' ' * 4 * tab


class NVD3Chart:
    """
    NVD3Chart Base class

    **Attributes**:

        * ``axislist`` - All X, Y axis list
        * ``charttooltip_dateformat`` - date fromat for tooltip if x-axis is in date format
        * ``charttooltip`` - Custom tooltip string
        * ``color_category`` - Defien color category (eg. category10, category20, category20c)
        * ``color_list`` - used by pieChart (eg. ['red', 'blue', 'orange'])
        * ``container`` - Place for graph
        * ``containerheader`` - Header for javascript code
        * ``count`` - chart count
        * ``custom_tooltip_flag`` - False / True
        * ``d3_select_extra`` -
        * ``date_flag`` - x-axis contain date format or not
        * ``dateformat`` - see https://github.com/mbostock/d3/wiki/Time-Formatting
        * ``header_css`` - False / True
        * ``header_js`` - Custom tooltip string
        * ``height`` - Set graph height
        * ``htmlcontent`` - Contain the htmloutput
        * ``htmlheader`` - Contain the html header
        * ``jschart`` - Javascript code as string
        * ``margin_bottom`` - set the bottom margin
        * ``margin_left`` - set the left margin
        * ``margin_right`` - set the right margin
        * ``margin_top`` - set the top margin
        * ``model`` - set the model (ex. pieChart, LineWithFocusChart, MultiBarChart)
        * ``resize`` - False / True
        * ``series`` - Series are list of data that will be plotted
        * ``stacked`` - False / True
        * ``style`` - Special style
        * ``template_page_nvd3`` - template variable
        * ``width`` - Set graph width
        * ``x_axis_date`` - False / True
        * ``show_legend`` - False / True
        * ``show_labels`` - False / True
        * ``assets_directory`` directory holding the assets (./bower_components/)
    """
    count = 0
    dateformat = '%x'
    series = []
    axislist = {}
    style = ''
    htmlcontent = ''
    htmlheader = ''
    height = None
    width = None
    margin_bottom = None
    margin_left = None
    margin_right = None
    margin_top = None
    model = ''
    d3_select_extra = ''
    x_axis_date = False
    resize = False
    stacked = False
    template_page_nvd3 = None
    container = None
    containerheader = ''
    jschart = None
    custom_tooltip_flag = False
    date_flag = False
    charttooltip = ''
    tooltip_condition_string = ''
    color_category = 'category10'  # category10, category20, category20c
    color_list = []  # for pie chart
    tag_script_js = True
    charttooltip_dateformat = None
    x_axis_format = ''
    show_legend = True
    show_labels = True
    assets_directory = './bower_components/'

    def __init__(self, **kwargs):
        """
        Constructor
        """
        #set the model
        self.model = self.__class__.__name__

        #Init Data
        self.series = []
        self.axislist = {}
        self.template_page_nvd3 = Template(template_page_nvd3)
        self.template_content_nvd3 = Template(template_content_nvd3)
        self.charttooltip_dateformat = '%d %b %Y'

        self.name = kwargs.get('name', self.model)
        self.jquery_on_ready = kwargs.get('jquery_on_ready', False)
        self.color_category = kwargs.get('color_category', None)
        self.color_list = kwargs.get('color_list', None)
        self.margin_bottom = kwargs.get('margin_bottom', 20)
        self.margin_left = kwargs.get('margin_left', 60)
        self.margin_right = kwargs.get('margin_right', 60)
        self.margin_top = kwargs.get('margin_top', 30)
        self.stacked = kwargs.get('stacked', False)
        self.resize = kwargs.get('resize', False)
        self.show_legend = kwargs.get('show_legend', True)
        self.show_labels = kwargs.get('show_labels', True)
        self.tag_script_js = kwargs.get('tag_script_js', True)
        self.chart_attr = kwargs.get("chart_attr", {})
        self.assets_directory = kwargs.get('assets_directory', './bower_components/')

        #CDN http://cdnjs.com/libraries/nvd3/ needs to make sure it's up to date
        self.header_css = [
            '<link href="%s" rel="stylesheet">\n' % h for h in
            (
                self.assets_directory + 'nvd3/src/nv.d3.css',
            )
        ]

        self.header_js = [
            '<script src="%s"></script>\n' % h for h in
            (
                self.assets_directory + 'd3/d3.min.js',
                self.assets_directory + 'nvd3/nv.d3.min.js'
            )
        ]

    def add_serie(self, y, x, name=None, extra={}, **kwargs):
        """
        add serie - Series are list of data that will be plotted
        y {1, 2, 3, 4, 5} / x {1, 2, 3, 4, 5}

        **Attributes**:

            * ``name`` - set Serie name
            * ``x`` - x-axis data
            * ``y`` - y-axis data

            kwargs:

            * ``shape`` - for scatterChart, you can set different shapes (circle, triangle etc...)
            * ``size`` - for scatterChart, you can set size of different shapes
            * ``type`` - for multiChart, type should be bar
            * ``bar`` - to display bars in Chart
            * ``color_list`` - define list of colors which will be used by pieChart
            * ``color`` - set axis color
            * ``disabled`` -

            extra:

            * ``tooltip`` - set tooltip flag
            * ``date_format`` - set date_format for tooltip if x-axis is in date format

        """
        if not name:
            name = "Serie %d" % (len(self.series) + 1)

        # For scatterChart shape & size fields are added in serie
        if 'shape' in kwargs or 'size' in kwargs:
            csize = kwargs.get('size', 1)
            cshape = kwargs.get('shape', 'circle')

            serie = [{
                'x': x[i],
                'y': y,
                'shape': cshape,
                'size': csize[i] if isinstance(csize, list) else csize
            } for i, y in enumerate(y)]
        else:
            if self.model == 'pieChart':
                serie = [{'label': x[i], 'value': y} for i, y in enumerate(y)]
            elif self.model == 'linePlusBarWithFocusChart':
                serie = [[x[i], y] for i, y in enumerate(y)]
            else:
                serie = [{'x': x[i], 'y': y} for i, y in enumerate(y)]

        data_keyvalue = {'values': serie, 'key': name}

        #multiChart
        #Histogram type='bar' for the series
        if 'type' in kwargs and kwargs['type']:
            data_keyvalue['type'] = kwargs['type']

        if self.model == 'pieChart':
            if 'color_list' in extra and extra['color_list']:
                self.color_list = extra['color_list']

        #Define on which Y axis the serie is related
        #a chart can have 2 Y axis, left and right, by default only one Y Axis is used
        if 'yaxis' in kwargs and kwargs['yaxis']:
            data_keyvalue['yAxis'] = kwargs['yaxis']
        else:
            if self.model != 'pieChart' and self.model != 'linePlusBarWithFocusChart':
                data_keyvalue['yAxis'] = '1'

        if 'bar' in kwargs and kwargs['bar']:
            data_keyvalue['bar'] = 'true'

        if 'disabled' in kwargs and kwargs['disabled']:
            data_keyvalue['disabled'] = 'true'

        if 'color' in extra and extra['color']:
            data_keyvalue['color'] = extra['color']

        if extra.get('date_format'):
            self.charttooltip_dateformat = extra['date_format']

        if extra.get('tooltip'):
            self.custom_tooltip_flag = True

            if self.model != 'pieChart':
                _start = extra['tooltip']['y_start']
                _end = extra['tooltip']['y_end']
                _start = ("'" + str(_start) + "' + ") if _start else ''
                _end = (" + '" + str(_end) + "'") if _end else ''

                if self.model == 'linePlusBarChart' or self.model == 'linePlusBarWithFocusChart':
                    self.tooltip_condition_string += stab(3) + "if(key.indexOf('" + name + "') > -1 ){\n" +\
                        stab(4) + "var y = " + _start + " String(graph.point.y) " + _end + ";\n" +\
                        stab(3) + "}\n"
                elif self.model == 'cumulativeLineChart':
                    self.tooltip_condition_string += stab(3) + "if(key == '" + name + "'){\n" +\
                        stab(4) + "var y = " + _start + " String(e) " + _end + ";\n" +\
                        stab(3) + "}\n"
                else:
                    self.tooltip_condition_string += stab(3) + "if(key == '" + name + "'){\n" +\
                        stab(4) + "var y = " + _start + " String(graph.point.y) " + _end + ";\n" +\
                        stab(3) + "}\n"

            if self.model == 'pieChart':
                _start = extra['tooltip']['y_start']
                _end = extra['tooltip']['y_end']
                _start = ("'" + str(_start) + "' + ") if _start else ''
                _end = (" + '" + str(_end) + "'") if _end else ''
                self.tooltip_condition_string += \
                    "var y = " + _start + " String(y) " + _end + ";\n"

        self.series.append(data_keyvalue)

    def set_graph_height(self, height):
        """Set Graph height"""
        self.height = str(height)

    def set_graph_width(self, width):
        """Set Graph width"""
        self.width = str(width)

    def set_containerheader(self, containerheader):
        """Set containerheader"""
        self.containerheader = containerheader

    def set_date_flag(self, date_flag=False):
        """Set date falg"""
        self.date_flag = date_flag

    def set_custom_tooltip_flag(self, custom_tooltip_flag):
        """Set custom_tooltip_flag & date_flag"""
        self.custom_tooltip_flag = custom_tooltip_flag

    def __str__(self):
        """return htmlcontent"""
        self.buildhtml()
        return self.htmlcontent

    def buildcontent(self):
        """Build HTML content only, no header or body tags. To be useful this
        will usually require the attribute `juqery_on_ready` to be set which
        will wrap the js in $(function(){<regular_js>};)
        """
        self.buildcontainer()
        self.buildjschart()
        self.htmlcontent = self.template_content_nvd3.substitute(container=self.container,
                                                                 jschart=self.jschart)

    def buildhtml(self):
        """Build the HTML page
        Create the htmlheader with css / js
        Create html page
        Add Js code for nvd3
        """
        self.buildhtmlheader()
        self.buildcontainer()
        self.buildjschart()

        self.htmlcontent = self.template_page_nvd3.substitute(header=self.htmlheader,
                                                              container=self.container,
                                                              jschart=self.jschart)

    def buildhtmlheader(self):
        """generate HTML header content"""
        self.htmlheader = ''
        for css in self.header_css:
            self.htmlheader += css
        for js in self.header_js:
            self.htmlheader += js

    def buildcontainer(self):
        """generate HTML div"""
        self.container = self.containerheader
        #Create SVG div with style
        if self.width:
            if self.width[-1] != '%':
                self.style += 'width:%spx;' % self.width
            else:
                self.style += 'width:%s;' % self.width
        if self.height:
            if self.height[-1] != '%':
                self.style += 'height:%spx;' % self.height
            else:
                self.style += 'height:%s;' % self.height
        if self.style:
            self.style = 'style="%s"' % self.style

        self.container += '<div id="%s"><svg %s></svg></div>\n' % (self.name, self.style)

    def build_custom_tooltip(self):
        """generate custom tooltip for the chart"""
        if self.custom_tooltip_flag:
            if not self.date_flag:
                if self.model == 'pieChart':
                    self.charttooltip = stab(2) + "chart.tooltipContent(function(key, y, e, graph) {\n" + \
                        stab(3) + "var x = String(key);\n" +\
                        stab(3) + self.tooltip_condition_string +\
                        stab(3) + "tooltip_str = '<center><b>'+x+'</b></center>' + y;\n" +\
                        stab(3) + "return tooltip_str;\n" + \
                        stab(2) + "});\n"
                else:
                    self.charttooltip = stab(2) + "chart.tooltipContent(function(key, y, e, graph) {\n" + \
                        stab(3) + "var x = String(graph.point.x);\n" +\
                        stab(3) + "var y = String(graph.point.y);\n" +\
                        self.tooltip_condition_string +\
                        stab(3) + "tooltip_str = '<center><b>'+key+'</b></center>' + y + ' at ' + x;\n" +\
                        stab(3) + "return tooltip_str;\n" + \
                        stab(2) + "});\n"
            else:
                self.charttooltip = stab(2) + "chart.tooltipContent(function(key, y, e, graph) {\n" + \
                    stab(3) + "var x = d3.time.format('%s')(new Date(parseInt(graph.point.x)));\n" \
                    % self.charttooltip_dateformat +\
                    stab(3) + "var y = String(graph.point.y);\n" +\
                    self.tooltip_condition_string +\
                    stab(3) + "tooltip_str = '<center><b>'+key+'</b></center>' + y + ' on ' + x;\n" +\
                    stab(3) + "return tooltip_str;\n" + \
                    stab(2) + "});\n"

    def buildjschart(self):
        """generate javascript code for the chart"""

        self.jschart = ''
        if self.tag_script_js:
            self.jschart += '\n<script>\n'

        self.jschart += stab()

        if self.jquery_on_ready:
            self.jschart += '$(function(){'

        self.jschart += 'nv.addGraph(function() {\n'

        self.jschart += stab(2) + 'var chart = nv.models.%s();\n' % self.model

        if self.model != 'pieChart' and not self.color_list:
            if self.color_category:
                self.jschart += stab(2) + 'chart.color(d3.scale.%s().range());\n' % self.color_category

        if self.stacked:
            self.jschart += stab(2) + "chart.stacked(true);"

        self.jschart += stab(2) + \
          'chart.margin({top: %s, right: %s, bottom: %s, left: %s})\n' % \
          (self.margin_top, self.margin_right, \
           self.margin_bottom, self.margin_left)

        """
        We want now to loop through all the defined Axis and add:
            chart.y2Axis
                .tickFormat(function(d) { return '$' + d3.format(',.2f')(d) });
        """
        if self.model != 'pieChart':
            for axis_name, a in list(self.axislist.items()):
                self.jschart += stab(2) + "chart.%s\n" % axis_name
                for attr, value in list(a.items()):
                    self.jschart += stab(3) + ".%s(%s);\n" % (attr, value)

        if self.width:
            self.d3_select_extra += ".attr('width', %s)\n" % self.width
        if self.height:
            self.d3_select_extra += ".attr('height', %s)\n" % self.height

        if self.model == 'pieChart':
            datum = "data_%s[0].values" % self.name
        else:
            datum = "data_%s" % self.name

        # add custom tooltip string in jschart
        # default condition (if build_custom_tooltip is not called explicitly with date_flag=True)
        if self.tooltip_condition_string == '':
            self.tooltip_condition_string = 'var y = String(graph.point.y);\n'

        self.build_custom_tooltip()
        self.jschart += self.charttooltip

        # the shape attribute in kwargs is not applied when 
        # not allowing other shapes to be rendered 
        if self.model == 'scatterChart':
           self.jschart += 'chart.scatter.onlyCircles(false);'

        if self.model != 'discreteBarChart':
            if self.show_legend:
                self.jschart += stab(2) + "chart.showLegend(true);\n"
            else:
                self.jschart += stab(2) + "chart.showLegend(false);\n"

        #showLabels only supported in pieChart
        if self.model == 'pieChart':
            if self.show_labels:
                self.jschart += stab(2) + "chart.showLabels(true);\n"
            else:
                self.jschart += stab(2) + "chart.showLabels(false);\n"

        # add custom chart attributes
        for attr, value in self.chart_attr.items():
            self.jschart += stab(2) + "chart.%s(%s);\n" % (attr, value)

        #Inject data to D3
        self.jschart += stab(2) + "d3.select('#%s svg')\n" % self.name + \
            stab(3) + ".datum(%s)\n" % datum + \
            stab(3) + ".transition().duration(500)\n" + \
            stab(3) + self.d3_select_extra + \
            stab(3) + ".call(chart);\n\n"

        if self.resize:
            self.jschart += stab(1) + "nv.utils.windowResize(chart.update);\n"
        self.jschart += stab(1) + "return chart;\n});"

        if self.jquery_on_ready:
            self.jschart += "\n});"

        #Include data
        series_js = json.dumps(self.series)

        if self.model == 'linePlusBarWithFocusChart':
            append_to_data = ".map(function(series) {" + \
                "series.values = series.values.map(function(d) { return {x: d[0], y: d[1] } });" + \
                "return series; })"
            self.jschart += """data_%s=%s%s;\n""" % (self.name, series_js, append_to_data)
        else:
            self.jschart += """data_%s=%s;\n""" % (self.name, series_js)

        if self.tag_script_js:
            self.jschart += "</script>"

    def create_x_axis(self, name, label=None, format=None, date=False, custom_format=False):
        """
        Create X-axis
        """
        axis = {}
        if custom_format and format:
            axis['tickFormat'] = format
        else:
            if format:
                if format == 'AM_PM':
                    axis['tickFormat'] = "function(d) { return get_am_pm(parseInt(d)); }"
                else:
                    axis['tickFormat'] = "d3.format(',%s')" % format

        if label:
            axis['axisLabel'] = label

        #date format : see https://github.com/mbostock/d3/wiki/Time-Formatting
        if date:
            self.dateformat = format
            axis['tickFormat'] = "function(d) { return d3.time.format('%s')(new Date(parseInt(d))) }\n" % self.dateformat
            #flag is the x Axis is a date
            if name[0] == 'x':
                self.x_axis_date = True

        #Add new axis to list of axis
        self.axislist[name] = axis

    def create_y_axis(self, name, label=None, format=None, custom_format=False):
        """
        Create Y-axis
        """
        axis = {}

        if custom_format and format:
            axis['tickFormat'] = format
        else:
            if format:
                axis['tickFormat'] = "d3.format(',%s')" % format

        if label:
            axis['axisLabel'] = label

        #Add new axis to list of axis
        self.axislist[name] = axis


def _main():
    """
    Parse options and process commands
    """
    # Parse arguments
    usage = "usage: nvd3.py [options]"
    parser = OptionParser(usage=usage, version="python-nvd3 - Charts generator with nvd3.js and d3.js")
    parser.add_option("-q", "--quiet",
                      action="store_false", dest="verbose", default=True,
                      help="don't print messages to stdout")

    (options, args) = parser.parse_args()


if __name__ == '__main__':
    _main()
