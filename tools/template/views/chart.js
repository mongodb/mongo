var AmpersandView = require('ampersand-view'),
    Rickshaw = require('rickshaw'),
    d3 = require('d3'),
    $ = require('jquery'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  template: require('./templates/chart.jade'),

  // renderChartRickshaw: function () {
  //   // make chart
  //   var graph = new Rickshaw.Graph({
  //     height: 600,
  //     element: document.querySelector('#graph'),
  //     renderer: 'line',
  //     series: this.model.series
  //   });

  //   graph.render();

  //   var hoverDetail = new Rickshaw.Graph.HoverDetail( {
  //     graph: graph
  //   });

  //   // var shelving = new Rickshaw.Graph.Behavior.Series.Toggle( {
  //   //   graph: graph,
  //   //   legend: legend
  //   // } );

  //   var y_ticks = new Rickshaw.Graph.Axis.Y( {
  //     graph: graph,
  //     orientation: 'left',
  //     tickFormat: Rickshaw.Fixtures.Number.formatKMBT,
  //     element: this.query('#y-axis')
  //   });
  //   y_ticks.render();

  //   var axes = new Rickshaw.Graph.Axis.Time( {
  //     graph: graph
  //   });
  //   axes.render();
  // },

  renderChartD3: function () {
    var margin = {top: 20, right: 80, bottom: 60, left: 150},
        width = 1200 - margin.left - margin.right,
        height = 600 - margin.top - margin.bottom;

    // var parseDate = d3.time.format("%Y%m%d").parse;

    var x = d3.scale.linear()
        .range([0, width]);

    var y = d3.scale.linear()
        .range([height, 0]);

    var xAxis = d3.svg.axis()
        .scale(x)
        .orient("bottom");

    var yAxis = d3.svg.axis()
        .scale(y)
        .orient("left");

    var line = d3.svg.line()
        .interpolate("cubic")
        .x(function(d) { return x(d.x); })
        .y(function(d) { return y(d.y); });

    var svg = d3.select('#graph').append("svg")
        .attr("width", width + margin.left + margin.right)
        .attr("height", height + margin.top + margin.bottom)
      .append("g")
        .attr("transform", "translate(" + margin.left + "," + margin.top + ")");

    var series = this.model.series.filter(function (serie) {
      return serie.selected;
    })

    // data.forEach(function(d) {
    //   d.date = parseDate(d.date);
    // });

    x.domain([
      d3.min(series, function (s) { return d3.min(s.data, function (v) {return v.x; }); }),
      d3.max(series, function (s) { return d3.max(s.data, function (v) {return v.x; }); })
    ]);

    y.domain([
      d3.min(series, function (s) { return d3.min(s.data, function (v) {return v.y; }); }),
      d3.max(series, function (s) { return d3.max(s.data, function (v) {return v.y; }); })
    ]);

    svg.append("g")
        .attr("class", "x axis")
        .attr("transform", "translate(0," + height + ")")
        .call(xAxis);

    svg.append("g")
        .attr("class", "y axis")
        .call(yAxis);

    var paths = svg.selectAll(".serie")
        .data(series)
      .enter().append("g")
        .attr("class", "serie");

    paths.append("path")
        .attr("class", "line")
        .attr("d", function(d) { return line(d.data); })
        .style("stroke", function(d) { return d.color; });

    paths.selectAll(".point")
      .data(function (serie) { return serie.data.map( function(d) { return {x: d.x, y:d.y, c:serie.color }}); })
      .enter().append("circle")
        .attr("class", "point")
        .attr("cx", function (d) { return x(d.x); })
        .attr("cy", function (d) { return y(d.y); })
        .attr("r", "3px")
        .style("fill", function (d) { return d.c; });

    // series.append("text")
    //     .datum(function(d) { return {name: d.name, value: d.data[d.data.length - 1]}; })
    //     .attr("transform", function(d) { return "translate(" + x(d.value.x) + "," + y(d.value.y) + ")"; })
    //     .attr("x", 3)
    //     .attr("dy", ".35em")
    //     .text(function(d) { return d.name; });
  },

  render: function () {
    this.renderWithTemplate(this.model);
    // this.renderChartRickshaw();
    this.renderChartD3();
  },

  redraw: function () {
    $('#graph').empty();
    this.renderChartD3();
  }
});
