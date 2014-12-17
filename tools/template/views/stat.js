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
  clicked: function () {
    this.model.toggle('selected');
  }
});
