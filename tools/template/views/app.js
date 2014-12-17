var AmpersandView = require('ampersand-view'),
    SidebarView = require('./sidebar'),
    ChartView = require('./chart');

var AppView = module.exports = AmpersandView.extend({
  template: require('./templates/app.jade'),
  autorender: true,
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
        return new ChartView({
          el: el,
          model: this.model.chart
        });
      }
    }
  }
});
