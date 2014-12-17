var AmpersandState = require('ampersand-state'),
    AmpersandCollection = require('ampersand-collection'),
    Search = require('./search'),
    Panel = require('./panel'),
    debug = require('debug')('model:sidebar');


var PanelCollection = AmpersandCollection.extend({
  model: Panel
});

var Sidebar = module.exports = AmpersandState.extend({
  children: {
    search: Search,
  },
  collections: {
    panels: PanelCollection
  }
});
