#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart, stab


class linePlusBarWithFocusChart(NVD3Chart):
    """
    A linePlusBarWithFocusChart Chart is a type of chart which displays information
    as a series of data points connected by straight line segments
    and with some series with rectangular bars with lengths proportional
    to the values that they represent

    .. image:: ../_static/screenshot/linePlusBarWithFocusChart.png

    Python example::

        from nvd3 import linePlusBarWithFocusChart
        chart = linePlusBarWithFocusChart(name='linePlusBarChart', x_is_date=True, x_axis_format="%d %b %Y")

        xdata = [1365026400000000, 1365026500000000, 1365026600000000]
        ydata = [-6, 5, -1]
        y2data = [36, 55, 11]
        kwargs = {}
        kwargs['bar'] = True
        extra_serie = {"tooltip": {"y_start": "There is ", "y_end": " calls"},
                       "date_format": "%d %b %Y %H:%S" }
        chart.add_serie(name="Serie 1", y=ydata, x=xdata, extra=extra_serie, **kwargs)

        extra_serie = {"tooltip": {"y_start": "There is ", "y_end": " min"}}
        chart.add_serie(name="Serie 2", y=y2data, x=xdata, extra=extra_serie)
        chart.buildhtml()

    Javascript generated::

        data_linePlusBarWithFocusChart = [
            {
                "key" : "Quantity" ,
                "bar": true,
                "values" : [ [ 1136005200000 , 1271000.0] , [ 1138683600000 , 1271000.0] , ]
            },
            {
                "key" : "Price" ,
                "values" : [ [ 1136005200000 , 71.89] , [ 1138683600000 , 75.51]]
            }
        ].map(function(series) {
            series.values = series.values.map(function(d) { return {x: d[0], y: d[1] } });
            return series;
        });

        nv.addGraph(function() {
            var chart = nv.models.linePlusBarWithFocusChart()
                .margin({top: 30, right: 60, bottom: 50, left: 70})
                .x(function(d,i) { return i })
                .color(d3.scale.category10().range());

            chart.xAxis.tickFormat(function(d) {

                var dx = testdata[0].values[d] && testdata[0].values[d].x || 0;
                if (dx > 0) {
                    return d3.time.format('%x')(new Date(dx))
                }
                return null;
            });

            chart.x2Axis.tickFormat(function(d) {
                var dx = testdata[0].values[d] && testdata[0].values[d].x || 0;
                return d3.time.format('%x')(new Date(dx))
            });

            chart.y1Axis.tickFormat(d3.format(',f'));

            chart.y3Axis.tickFormat(d3.format(',f'));

            chart.y2Axis.tickFormat(function(d) { return '$' + d3.format(',.2f')(d) });

            chart.y4Axis.tickFormat(function(d) { return '$' + d3.format(',.2f')(d) });

            chart.bars.forceY([0]);
            chart.bars2.forceY([0]);
            //chart.lines.forceY([0]);
            nv.log(testdata);
            d3.select('#linePlusBarWithFocusChart svg')
                .datum(testdata)
                .call(chart);

            nv.utils.windowResize(chart.update);

            return chart;
            });
    """
    def __init__(self, **kwargs):
        NVD3Chart.__init__(self, **kwargs)
        height = kwargs.get('height', 450)
        width = kwargs.get('width', None)

        if kwargs.get('x_is_date', False):
            self.set_date_flag(True)

            with_focus_chart_1 = """function(d) {
                var dx = data_%s[0].values[d] && data_%s[0].values[d].x || 0;
                if (dx > 0) { return d3.time.format('%s')(new Date(dx)) }
                return null;
            }""" % (self.name, self.name, kwargs.get('x_axis_format', '%d %b %Y %H %S'))
            self.create_x_axis('xAxis', format=with_focus_chart_1, date=False, custom_format=True)

            with_focus_chart_2 = """function(d) {
                var dx = data_%s[0].values[d] && data_%s[0].values[d].x || 0;
                return d3.time.format('%s')(new Date(dx));
            }""" % (self.name, self.name, kwargs.get('x_axis_format', '%d %b %Y %H %S'))

            self.create_x_axis('x2Axis', format=with_focus_chart_2, date=False, custom_format=True)

            self.set_custom_tooltip_flag(True)
        else:
            self.create_x_axis('xAxis', format=".2f")

        self.create_y_axis('y1Axis', format="f")
        self.create_y_axis('y3Axis', format="f")

        self.create_y_axis('y2Axis', format="function(d) { return d3.format(',.2f')(d) }", custom_format=True)

        self.create_y_axis('y4Axis', format="function(d) { return d3.format(',.2f')(d) }", custom_format=True)

        # must have a specified height, otherwise it superimposes both chars
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)

    def buildjschart(self):
        NVD3Chart.buildjschart(self)

        string_jschart = '\n' + stab(2) + 'chart.margin({top: 30, right: 60, bottom: 50, left: 70})\n' + \
            stab(3) + '.x(function(d,i) { return i });\n'
        if self.width:
            string_jschart += stab(2) + 'chart.width(%s);\n' % self.width
        if self.height:
            string_jschart += stab(2) + 'chart.height(%s);\n' % self.height

        start_index = self.jschart.find('.linePlusBarWithFocusChart();')
        string_len = len('.linePlusBarWithFocusChart();')
        replace_index = start_index + string_len
        if start_index > 0:
            self.jschart = self.jschart[:replace_index] + string_jschart + self.jschart[replace_index:]
