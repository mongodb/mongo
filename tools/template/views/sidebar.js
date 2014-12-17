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
  },
  filterPanels: function (search) {
    _.each(this.panelViews.views, function (view) {
      view.filterStats(search);
    });
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
