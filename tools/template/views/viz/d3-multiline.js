var d3 = require('d3'),
    debug = require('debug')('viz:d3-multiline');

module.exports = function(opts) {
    var margin = {
      top: 20,
      right: 380,
      bottom: 60,
      left: 100
    },
    width = opts.width - margin.left - margin.right,
    height = opts.height - margin.top - margin.bottom,
    data = opts.data,
    el = opts.el;

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
      .interpolate("monotone")
      .tension(0.8)
      .x(function(d) { return x(d.x); })
      .y(function(d) { return y(d.y); });

    var svg = d3.select(el)
      .append('g')
      .attr("transform", "translate(" + margin.left + "," + margin.top + ")");
    
    svg.attr({width: width, height: height});

    var series = data;
    
    // no data to plot    
    if (series.length === 0) return;

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
}

