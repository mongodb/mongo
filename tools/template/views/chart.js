var AmpersandView = require('ampersand-view'),
    Rickshaw = require('rickshaw'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  template: require('./templates/chart.jade'),
  render: function () {
    this.renderWithTemplate(this.model);

    debug(this.model.series);

    // make chart
    var graph = new Rickshaw.Graph({
      height: 600,
      element: document.querySelector('#graph'),
      renderer: 'line',
      series: this.model.series
    });

    graph.render();

    var hoverDetail = new Rickshaw.Graph.HoverDetail( {
      graph: graph
    });

    // var shelving = new Rickshaw.Graph.Behavior.Series.Toggle( {
    //   graph: graph,
    //   legend: legend
    // } );

    var y_ticks = new Rickshaw.Graph.Axis.Y( {
      graph: graph,
      orientation: 'left',
      tickFormat: Rickshaw.Fixtures.Number.formatKMBT,
      element: this.query('#y-axis')
    });
    y_ticks.render();

    var axes = new Rickshaw.Graph.Axis.Time( {
      graph: graph
    });
    axes.render();
  }
});
