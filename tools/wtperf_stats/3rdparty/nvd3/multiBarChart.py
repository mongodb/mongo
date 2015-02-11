#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart


class multiBarChart(NVD3Chart):
    """
    A multiple bar graph contains comparisons of two or more categories or bars.
    One axis represents a quantity and the other axis identifies a specific feature
    about the categories. Reading a multiple bar graph includes looking at extremes
    (tallest/longest vs. shortest) in each grouping.

    .. image:: ../_static/screenshot/multiBarChart.png

    Python example::

        from nvd3 import multiBarChart
        chart = multiBarChart(name='multiBarChart', height=400, width=400)
        xdata = [0, 1, 3, 4]
        ydata = [6, 12, 9, 16]

        extra_serie = {"tooltip": {"y_start": "", "y_end": " balls"}}
        chart.add_serie(name="Serie 1", y=ydata, x=xdata)
        chart.buildhtml()

    Javascript generated::

        data_MultiBarChart = [{ "key" : "Serie 1",
           "values" : [
                { "x" : 0
                  "y" : 6
                },
                { "x" : 1,
                  "y" : 12
                },
                { "x" : 3,
                  "y" : 9
                },
              ],
            "yAxis" : "1"
        }]

        nv.addGraph(function() {
            var chart = nv.models.multiBarChart();
            chart.xAxis
                .tickFormat(d3.format(',.2f'));
            chart.yAxis
                .tickFormat(d3.format(',.2f'));
            chart.tooltipContent(function(key, y, e, graph) {
                var x = String(graph.point.x);
                var y = String(graph.point.y);
                if(key == 'Serie 1'){
                    var y =  String(graph.point.y)  + ' balls';
                }
                tooltip_str = '<center><b>'+key+'</b></center>' + y + ' at ' + x;
                return tooltip_str;
            });
            d3.select('#MultiBarChart svg')
                .datum(data_MultiBarChart)
                .transition()
                .duration(500)
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
                               format=kwargs.get('x_axis_format', '%d %b %Y'),
                               date=True)
            self.set_custom_tooltip_flag(True)
        else:
            self.create_x_axis('xAxis', format=kwargs.get('x_axis_format', '.2f'))
        self.create_y_axis('yAxis', format=kwargs.get('y_axis_format', '.2f'))
        # must have a specified height, otherwise it superimposes both chars
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)
