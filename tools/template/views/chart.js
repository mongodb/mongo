var AmpersandView = require('ampersand-view'),
    debug = require('debug')('view:chart');

var ChartView = module.exports = AmpersandView.extend({
  template: require('./templates/chart.jade'),
  autorender: true
});
