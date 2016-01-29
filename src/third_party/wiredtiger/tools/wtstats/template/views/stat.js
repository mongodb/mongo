var AmpersandView = require('ampersand-view'),
    debug = require('debug')('view:stat');

var StatView = module.exports = AmpersandView.extend({
  template: require('./templates/stat.jade'),
  render: function () {
    this.renderWithTemplate(this.model);
  },
  events: {
    'click': 'clicked',
  },
  bindings: {
    'model.selected': {
      type: 'booleanClass',
      hook: 'circle',
      yes: 'fa-circle',
      no: 'fa-circle-o'
    }
  },
  clicked: function (event) {
    // ignore shift+click, app handles those
    if (event.shiftKey) {
      this.model.app.toggleAllExcept(this.model);
    } else {
      this.model.app.clearSelectionState();
      this.model.toggle('selected');
    }
    this.parent.parent.statChanged(this, {all: event.shiftKey, propagate: true});
  }
});
