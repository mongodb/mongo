var AmpersandView = require('ampersand-view'),
    VizView = require('./viz'),
    d3 = require('d3'),
    $ = require('jquery'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  template: require('./templates/chart.jade'),

  render: function () {
    this.renderWithTemplate(this.model);

    this.renderSubview(new VizView({
      width: 'auto',
      height: 600,
      renderMode: 'svg',
      className: 'multiline',
      debounceRender: false,
      vizFn: require('./viz/d3-multiline'),
      data: {
        series: this.model.series.filter(function (s) { return s.selected; }),
        options: this.model.serialize()
      }
    }), '[data-hook=graph]');

  },

  redraw: function () {
    this.render();
    // $('#graph').empty();
    // this.renderChartD3();
  }
});
