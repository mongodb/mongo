var d3 = require('d3'),
    debug = require('debug')('viz:d3-multiline');

/**
 * helper function to move an element to the front
 */
d3.selection.prototype.moveToFront = function() {
  return this.each(function(){
  this.parentNode.appendChild(this);
  });
};

module.exports = function(opts) {
  
  function redraw(opts) {
    var transTime = 300;
    // var initial = (initial === undefined) || false;

    // recalculate width/height
    width = opts.width - margin.left - margin.right;
    height = opts.height - margin.top - margin.bottom;
    
    // new data and settings
    data = opts.data;
    options = data.options;
    series = data.series;

    if (series.length === 0) {
      svg.style('visibility', 'hidden');
      // return false;
    } else {
      svg.style('visibility', 'visible');
    }

    svg.attr({width: width, height: height});

    // update x domain
    x.range([0, width]);
    x.domain([
      d3.min(series, function (s) { return d3.min(s.data, function (v) {return v.x; }); }),
      d3.max(series, function (s) { return d3.max(s.data, function (v) {return v.x; }); })
    ]);

    // create new y axis and set domain
    if (options.ySetting === 'linear') {
      y = d3.scale.linear().range([height, 0]);
      y.domain([
        d3.min(series, function (s) { return d3.min(s.data, function (v) {return v.y; }); }),
        d3.max(series, function (s) { return d3.max(s.data, function (v) {return v.y; }); })
      ]);
    } else {
      y = d3.scale.log().clamp(true).range([height, 0]).nice();
      y.domain([
        0.1, d3.max(series, function (s) { return d3.max(s.data, function (v) {return v.y; }); })
      ]);
    }

    // redraw x and y axes
    svg.selectAll('.x')
      .transition().duration(transTime)
      .call(xAxis.scale(x));

    svg.selectAll('.y')
      .transition().duration(transTime)
      .call(yAxis.scale(y));      

    // transition data
    paths = svg.selectAll(".serie")
      .data(series, function (d) { return d.cid});

    paths.enter()
      .append("g")
        .attr("class", "serie")
        .append("path")
          .attr("class", "line")
          .style("stroke", function(d) { return d.color; });


    paths.exit().remove();

    paths.selectAll('path')
      .transition().duration(transTime)
      .attr("d", function(d) { return line(d.data); });

    circles = paths.selectAll(".point")
      .data(function (serie) { return serie.data.map( function(d) { return {x: d.x, y:d.y, c:serie.color }}); });

    circles.enter().append("circle")
      .attr("class", "point")
      .attr("r", "3px")
      .style("fill", function (d) { return d.c; });

    circles.exit().remove();

    circles
      .transition().duration(transTime)
      .attr("cx", function (d) { return x(d.x); })
      .attr("cy", function (d) { return y(d.y); })

    crosshairX
      .attr("x2", width);

    crosshairY
      .attr("y2", height);

    windshield
      .attr("width", width)
      .attr("height", height)
      .moveToFront();

  } // redraw

  function findClosest(mx, series) {
    var mxi = x.invert(mx);
    var i = bisect(series.data, mxi);
    var d0 = series.data[i - 1];
    var d1 = series.data[i];
    return (mxi - d0.x > d1.x - mxi) ? d1 : d0;
  }

  function mousemove() {
    var mouse = d3.mouse(this);
    var mx = mouse[0], 
        my = mouse[1];

    var dArr = series.map(function (serie) { return findClosest(mx, serie); });
    var dists = dArr.map(function (d) {
      return Math.sqrt(Math.pow(mx-x(d.x), 2) + Math.pow(my-y(d.y), 2));
    })
    var i = dists.indexOf(Math.min.apply(Math, dists));
    var serie = series[i];
    var d = dArr[i];

    focus
      .attr("transform", "translate(" + x(d.x) + "," + y(d.y) + ")")
      .moveToFront();

    focus.select("circle")
      .attr('stroke', serie.color);

    focus.select("text.name")
      .text(serie.name);
    
    focus.select("text.value")
      .text(d3.format(",")(d.y));

    // focus.select("rect.marker")
    //   .style("fill", serie.color);

    crosshairX
      .attr("y1", y(d.y))
      .attr("y2", y(d.y));

    crosshairY
      .attr("x1", x(d.x))
      .attr("x2", x(d.x));
  }

  // --- initial setup (only called once) ---
  var margin = {
      top: 80,
      right: 20,
      bottom: 60,
      left: 100
    },
    width = opts.width - margin.left - margin.right,
    height = opts.height - margin.top - margin.bottom,
    el = opts.el;

  var options, series; // initialized in redraw

  var bisect = d3.bisector(function(d) { return d.x; }).left;

  var x = d3.scale.linear(); 
  var y; // initialized in redraw

  var xAxis = d3.svg.axis()
    .scale(x)
    .orient("bottom");

  var yAxis = d3.svg.axis()
    .orient("left");

  var line = d3.svg.line()
    .interpolate("monotone")
    .tension(0.8)
    .x(function(d) { return x(d.x); })
    .y(function(d) { return y(d.y); });

  var svg = d3.select(el)
    .append('g')
      .attr("transform", "translate(" + margin.left + "," + margin.top + ")");
  
  // svg.attr({width: width, height: height});

  svg.append("g")
    .attr("class", "x axis")
    .attr("transform", "translate(0," + height + ")");

  svg.append("g")
    .attr("class", "y axis");

  var crosshairX = svg.append("line")
    .style("stroke", "#ddd")
    .style("display", "none")
    .attr("class", 'x')
    .attr("x1", 0);

  var crosshairY = svg.append("line")
    .style("stroke", "#ddd")
    .style("display", "none")
    .attr("class", 'x')
    .attr("y1", 0);

  // focus
  var focus = svg.append("g")
    .attr("class", "focus")
    .style("display", "none");

  focus.append("circle")
    .attr("r", 6)
    .attr("fill", "none")
    .attr("stroke-width", 2);

  // focus.append('rect')
  //   .attr("class", "marker")
  //   .attr("width", 6)
  //   .attr("height", 34)
  //   .attr("x", 9)
  //   .attr("y", -40);

  focus.append("text")
    .attr("class", "name")
    .attr('fill', 'black')
    .attr("x", 9)
    .attr("dy", "-.8em");
  
  focus.append("text")
    .attr("class", "value")
    .attr('fill', 'black')
    .attr("font-size", "20px")
    .attr("x", 8)
    .attr("dy", "-1.2em");

  // rect for mouseover
  var windshield = svg.append("rect")
    .attr("class", "windshield")
    .style("opacity", 0.)
    .on("mouseover", function() { 
      crosshairX.style("display", null);
      crosshairY.style("display", null);
      focus.style("display", null);
    })
    .on("mouseout", function() { 
      focus.style("display", "none"); 
      crosshairX.style("display", "none"); 
      crosshairY.style("display", "none"); 
    })
    .on("mousemove", mousemove);

  var paths, circles;  // initialized in redraw

  return redraw;
}

