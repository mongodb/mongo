#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart


class cumulativeLineChart(NVD3Chart):
    """
    A cumulative line chart is used when you have one important grouping representing
    an ordered set of data and one value to show, summed over time.

    .. image:: ../_static/screenshot/cumulativeLineChart.png

    Python example::

        from nvd3 import cumulativeLineChart
        chart = cumulativeLineChart(name='cumulativeLineChart', x_is_date=True)
        xdata = [1365026400000000, 1365026500000000, 1365026600000000]
        ydata = [-6, 5, -1]
        y2data = [36, 55, 11]

        extra_serie = {"tooltip": {"y_start": "There are ", "y_end": " calls"}}
        chart.add_serie(name="Serie 1", y=ydata, x=xdata, extra=extra_serie)

        extra_serie = {"tooltip": {"y_start": "", "y_end": " mins"}}
        chart.add_serie(name="Serie 2", y=y2data, x=xdata, extra=extra_serie)
        chart.buildhtml()

    Javascript generated::

        data_lineWithFocusChart = [
            {
               "key" : "Serie 1",
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
            var chart = nv.models.cumulativeLineChart();

            chart.xAxis
                .tickFormat(function(d) { return d3.time.format('%d %b %Y')(new Date(d)) });
            chart.y1Axis
                .tickFormat(d3.format('.1%'));
            chart.tooltipContent(function(key, y, e, graph) {
                var x = d3.time.format('%d %b %Y')(new Date(parseInt(graph.point.x)));
                var y = String(graph.point.y);
                if(key == 'Serie 1'){
                    var y = 'There are ' + String(e)  + ' calls';
                }
                if(key == 'Serie 2'){
                    var y =  String(e)  + ' mins';
                }
                tooltip_str = '<center><b>'+key+'</b></center>' + y + ' on ' + x;
                return tooltip_str;
            });
            d3.select('#cumulativeLineChart svg')
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
                               format=kwargs.get('x_axis_format', '%d %b %Y'),
                               date=True)
            self.set_custom_tooltip_flag(True)
        else:
            self.create_x_axis('xAxis', format=kwargs.get('x_axis_format', '.2f'))

        self.create_y_axis('yAxis', format=kwargs.get('y_axis_format', '.1%'))

        # must have a specified height, otherwise it superimposes both chars
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)
