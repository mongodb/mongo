#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart


class discreteBarChart(NVD3Chart):
    """
    A discrete bar chart or bar graph is a chart with rectangular bars with
    lengths proportional to the values that they represent.

    .. image:: ../_static/screenshot/discreteBarChart.png

    Python example::

        from nvd3 import discreteBarChart
        chart = discreteBarChart(name='discreteBarChart', height=400, width=400)

        xdata = ["A", "B", "C", "D", "E"]
        ydata = [3, 4, 0, -3, 5, 7]

        extra_serie = {"tooltip": {"y_start": "", "y_end": " balls"}}
        chart.add_serie(y=ydata, x=xdata, extra=extra_serie)
        chart.buildhtml()

    Javascript generated::

        data = [{ key: "Cumulative Return",
                  values: [
                    {
                      "label": "A",
                      "value" : 10
                    },
                    {
                      "label": "B",
                      "value" : 0
                    },
                    {
                      "label": "C",
                      "value" : -3
                    },
                  ]
                }]

        nv.addGraph(function() {
            var chart = nv.models.discreteBarChart()
                .x(function(d) { return d.label })
                .y(function(d) { return d.value })
                .showLabels(true);
            chart.tooltipContent(function(key, y, e, graph) {
                var x = String(graph.point.x);
                var y = String(graph.point.y);
                if(key == 'Serie 1'){
                    var y =  String(graph.point.y)  + ' balls';
                }
                tooltip_str = '<center><b>'+key+'</b></center>' + y + ' at ' + x;
                return tooltip_str;
            });
            d3.select("#div_id")
                .datum(data)
                .transition()
                .duration(1200)
                .call(chart);

            return chart;
        });
    """
    def __init__(self, **kwargs):
        NVD3Chart.__init__(self, **kwargs)
        self.name = kwargs.get('name', 'discreteBarChart')
        height = kwargs.get('height', 450)
        width = kwargs.get('width', None)

        if kwargs.get('x_is_date', False):
            self.set_date_flag(True)
            self.create_x_axis('xAxis',
                               format=kwargs.get('x_axis_format', "%d %b %Y %H %S"),
                               date=True)
        else:
            self.create_x_axis('xAxis', format=None)

        self.set_custom_tooltip_flag(True)

        # must have a specified height, otherwise it superimposes both charts
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)
