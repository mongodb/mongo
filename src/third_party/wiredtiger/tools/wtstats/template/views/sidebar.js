var AmpersandView = require('ampersand-view'),
    PanelView = require('./panel'),
    _ = require('lodash'),
    debug = require('debug')('view:sidebar');

var SidebarView = module.exports = AmpersandView.extend({
  props: {
    panelViews: 'object'
  },
  template: require('./templates/sidebar.jade'),
  events: {
    'click [data-hook=button]': 'clearClicked',
    'input [data-hook=input]': 'inputChanged'
  },
  bindings: {
    'model.search.content': {    
      type: 'value',
      hook: 'input'
    }
  },
  render: function () {
    this.renderWithTemplate(this.model);
    this.panelViews = this.renderCollection(this.model.panels, PanelView, this.queryByHook('panels'));
  },
  closeAndReset: function () {
    _.each(this.panelViews.views, function (view) {
      view.collapsibleClose();
      view.resetStats();
    });
  },
  clearClicked: function () {
    this.model.search.content = '';
    this.closeAndReset();
    this.queryByHook('button').blur();
  },
  filterPanels: function (search) {
    _.each(this.panelViews.views, function (view) {
      view.filterStats(search);
    });
  },
  statChanged: function(stat, options) {
    this.parent.statChanged(stat, options);
    if (options.all) {
      // inform all other panels that they need to update their indicators
      _.each(this.panelViews.views, function (pv) { 
        pv.statChanged(stat, {all: false, propagate: false});
      });
    }
  },
  inputChanged: _.debounce(function () {
    var content = this.queryByHook('input').value;
    this.model.search.content = content;

    if (content.trim() === '') {
      this.closeAndReset();
    } else {
      this.filterPanels(content);
    }
  }, 200, {leading: false, trailing: true})
});
