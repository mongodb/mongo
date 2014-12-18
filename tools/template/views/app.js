var AmpersandView = require('ampersand-view'),
    SidebarView = require('./sidebar'),
    ChartView = require('./chart'),
    debug = require('debug')('view:app');

var AppView = module.exports = AmpersandView.extend({
  template: require('./templates/app.jade'),
  autorender: true,
  props: {
    chartView: 'object'
  },
  subviews: {
    sidebar: {
      hook: 'sidebar',
      waitFor: 'model.sidebar',
      prepareView: function (el) {
        return new SidebarView({
          el: el,
          model: this.model.sidebar
        })
      }
    },
    chart: {
      hook: 'chart',
      waitFor: 'model.chart',
      prepareView: function (el) {
        this.chartView = new ChartView({
          el: el,
          model: this.model.chart
        });
        return this.chartView;
      }
    }
  },
  statChanged: function (stat) {
    this.chartView.redraw();
  }
});
