#!/usr/bin/python
# -*- coding: utf-8 -*-

"""
Python-nvd3 is a Python wrapper for NVD3 graph library.
NVD3 is an attempt to build re-usable charts and chart components
for d3.js without taking away the power that d3.js gives you.

Project location : https://github.com/areski/python-nvd3
"""

from .NVD3Chart import NVD3Chart, stab


class pieChart(NVD3Chart):
    """
    A pie chart (or a circle graph) is a circular chart divided into sectors,
    illustrating numerical proportion. In chart, the arc length of each sector
    is proportional to the quantity it represents.

    .. image:: ../_static/screenshot/pieChart.png

    Python example::

        from nvd3 import pieChart
        chart = pieChart(name='pieChart', color_category='category20c', height=400, width=400)

        xdata = ["Orange", "Banana", "Pear", "Kiwi", "Apple", "Strawberry", "Pineapple"]
        ydata = [3, 4, 0, 1, 5, 7, 3]

        extra_serie = {"tooltip": {"y_start": "", "y_end": " cal"}}
        chart.add_serie(y=ydata, x=xdata, extra=extra_serie)
        chart.buildhtml()

    Javascript generated::

        data = [{ key: "Cumulative Return",
                  values: [
                    {
                      "label": "One",
                      "value" : 29.765957771107
                    },
                    {
                      "label": "Two",
                      "value" : 0
                    },
                    {
                      "label": "Three",
                      "value" : 32.807804682612
                    },
                  ]
                }]

        nv.addGraph(function() {
            var chart = nv.models.pieChart()
              .x(function(d) { return d.label })
              .y(function(d) { return d.value })
              .showLabels(true);

            chart.color(d3.scale.category20c().range());

            chart.tooltipContent(function(key, y, e, graph) {
                var x = String(key);
                var y =  String(y)  + ' cal';
                tooltip_str = '<center><b>'+x+'</b></center>' + y;
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
        height = kwargs.get('height', 450)
        width = kwargs.get('width', None)

        self.create_x_axis('xAxis', format=None)
        self.create_y_axis('yAxis', format=None)
        # must have a specified height, otherwise it superimposes both chars
        if height:
            self.set_graph_height(height)
        if width:
            self.set_graph_width(width)

    def buildjschart(self):
        NVD3Chart.buildjschart(self)

        color_js = ''
        if self.color_list:
            color_js += "var mycolor = new Array();\n"
            color_count = 0
            for i in self.color_list:
                color_js += stab(2) + "mycolor[" + str(color_count) + "] = '" + i + "';\n"
                color_count = int(color_count) + 1

        # add mycolor var in js before nv.addGraph starts
        if self.color_list:
            start_js = self.jschart.find('nv.addGraph')
            #start_js_len = len('nv.addGraph')
            replace_index = start_js
            if start_js > 0:
                self.jschart = self.jschart[:replace_index] + color_js + self.jschart[replace_index:]

        pie_jschart = '\n' + stab(2) + 'chart.x(function(d) { return d.label })\n' + \
            stab(3) + '.y(function(d) { return d.value });\n'
        if self.width:
            pie_jschart += stab(2) + 'chart.width(%s);\n' % self.width
        if self.height:
            pie_jschart += stab(2) + 'chart.height(%s);\n' % self.height

        # add custom colors for pieChart
        if self.color_list and color_js:
            pie_jschart += stab(2) + 'chart.color(mycolor);\n'

        start_index = self.jschart.find('.pieChart();')
        string_len = len('.pieChart();')
        replace_index = start_index + string_len
        if start_index > 0:
            self.jschart = self.jschart[:replace_index] + pie_jschart + self.jschart[replace_index:]
