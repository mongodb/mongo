var AmpersandState = require('ampersand-state'),
    debug = require('debug')('model:search');

var Search = module.exports = AmpersandState.extend({
  props: {
    content: {
      type: 'string',
      default: ''
    }
  },
  derived: {
    empty: {
      deps: ['content'],
      fn: function () {
        return this.content === '';
      }
    }
  }
});
