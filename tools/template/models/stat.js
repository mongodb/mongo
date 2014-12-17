var AmpersandState = require('ampersand-state'),
    colors = require('./colors').getInstance(),
    debug = require('debug')('model:stat');

var Stat = module.exports = AmpersandState.extend({
  props: {
    name: {
      type: 'string',
      default: ''
    },
    short_name: {
      type: 'string',
      default: ''
    },
    selected: {
      type: 'boolean',
      default: false
    }, 
    visible: {
      type: 'boolean',
      default: true
    }
  },
  derived: {
    color: {
      cache: true,
      fn: function () {
        return colors(this.cid);
      }
    }
  }
});
