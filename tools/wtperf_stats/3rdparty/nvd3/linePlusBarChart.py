#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart


class linePlusBarChart(NVD3Chart):
    """
    A linePlusBarChart Chart is a type of chart which displays information
    as a series of data points connected by straight line segments
    and with some series with rectangular bars with lengths proportional
    to the values that they represent

    .. image:: ../_static/screenshot/linePlusBarChart.png

    Python example::

        from nvd3 import linePlusBarChart
        chart = linePlusBarChart(name='linePlusBarChart', x_is_date=True, x_axis_format="%d %b %Y")

        xdata = [1365026400000000, 1365026500000000, 1365026600000000]
        ydata = [-6, 5, -1]
        y2data = [36, 55, 11]

        extra_serie = {"tooltip": {"y_start": "There is ", "y_end": " calls"},
                       "date_format": "%d %b %Y %H:%S" }
        chart.add_serie(name="Serie 1", y=ydata, x=xdata, extra=extra_serie)

        extra_serie = {"tooltip": {"y_start": "There is ", "y_end": " min"}}
        chart.add_serie(name="Serie 2", y=y2data, x=xdata, extra=extra_serie)
        chart.buildhtml()

    Javascript generated::

        data_lineWithFocusChart = [
            {
               "key" : "Serie 1",
               "bar": "true",
               "values" : [
                    { "x" : "1365026400000000",
                      "y" : -6
                    },
                    { "x" : "1365026500000000",
                      "y" : -5
                    },
                    { "x" : "1365026600000000",
                      "y" : -1
                    },
                  ],
            },
            {
               "key" : "Serie 2",
               "values" : [
                    { "x" : "1365026400000000",
                      "y" : 34
                    },
                    { "x" : "1365026500000000",
                      "y" : 56
                    },
                    { "x" : "1365026600000000",
                      "y" : 32
                    },
                  ],
            }
        ]

        nv.addGraph(function() {
            var chart = nv.models.linePlusBarChart();

            chart.xAxis
                .tickFormat(function(d) { return d3.time.format('%d %b %Y')(new Date(d)) });
            chart.y1Axis
                .tickFormat(d3.format(',f'));
            chart.y2Axis
                .tickFormat(function(d) { return '$' + d3.format(',f')(d) });
            chart.tooltipContent(function(key, y, e, graph) {
                var x = d3.time.format('%d %b %Y %H:%S')(new Date(parseInt(graph.point.x)));
                var y = String(graph.point.y);
                if(key.indexOf('Serie 1') > -1 ){
                    var y = '$ ' +  String(graph.point.y) ;
                }
                if(key.indexOf('Serie 2') > -1 ){
                    var y =  String(graph.point.y)  + ' min';
                }
                tooltip_str = '<center><b>'+key+'</b></center>' + y + ' on ' + x;
                return tooltip_str;
            });
            d3.select('#linePlusBarChart svg')
                .datum(data_linePlusBarChart)
                .transition().duration(500)
                .attr('height', 350)
                .call(chart);
            return chart;
        });
    """
    def __init__(self, **kwargs):
        NVD3Chart.__init__(self, **kwargs)
        height = kwargs.get('height', 450)
        width = kwargs.get('width', None)

        if kwargs.get('x_is_date', False):
            self.set_date_flag(True)
            self.create_x_axis('xAxis',
                               format=kwargs.get('x_axis_format', '%d %b %Y %H %S'),
                               date=True)
            self.set_custom_tooltip_flag(True)
        else:
            self.create_x_axis('xAxis', format=kwargs.get('x_axis_format', '.2f'))

        self.create_y_axis('y1Axis', format="f")
        self.create_y_axis('y2Axis', format="function(d) { return d3.format(',f')(d) }", custom_format=True)

        # must have a specified height, otherwise it superimposes both chars
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)
