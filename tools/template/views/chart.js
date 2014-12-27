var AmpersandView = require('ampersand-view'),
    VizView = require('./viz'),
    d3 = require('d3'),
    $ = require('jquery'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  props: {
    vizView: {
      type: 'any',
      default: null
    }
  },
  template: require('./templates/chart.jade'),
  render: function () {
    if (!this.vizView) {
      this.renderWithTemplate(this.model);
      this.vizView = new VizView({
        width: 'auto',
        height: 600,
        renderMode: 'svg',
        className: 'multiline',
        debounceRender: false,
        vizFn: require('./viz/d3-multiline'),
        data: {
          series: this.model.series.filter(function (s) { return s.selected; }),
          model: this.model
        }
      });
    
      this.renderSubview(this.vizView, '[data-hook=graph]');
    } else {
      this.vizView.data = {
        series: this.model.series.filter(function (s) { return s.selected; }),
        model: this.model
      };
      this.vizView.redraw();
    }
  }
});
