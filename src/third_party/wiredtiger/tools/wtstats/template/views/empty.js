var AmpersandView = require('ampersand-view'),
    debug = require('debug')('view:empty');

var EmptyView = module.exports = AmpersandView.extend({
  template: require('./templates/empty.jade'),
  autorender: true
});

