var AmpersandView = require('ampersand-view'),
    VizView = require('./viz'),
    EmptyView = require('./empty'),
    d3 = require('d3'),
    $ = require('jquery'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  props: {
    vizView: {
      type: 'any',
      default: null
    },
    emptyView: {
      type: 'any',
      default: null
    },
    activeView: {
      type: 'string',
      default: 'empty',
      values: ['empty', 'viz']
    }
  },
  template: require('./templates/chart.jade'),
  bindings: {
    'activeView': {
      type: 'switch',
      cases: {
        'empty': '[data-hook=empty]',
        'viz'  : '[data-hook=graph]'
      }
    }
  },
  render: function () {

    this.activeView = this.model.empty ? 'empty' : 'viz';

    // first time, render view and create both subviews
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
      this.emptyView = new EmptyView();

      this.renderSubview(this.emptyView, '[data-hook=empty]');
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
