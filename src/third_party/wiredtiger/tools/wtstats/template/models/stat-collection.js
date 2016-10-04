var AmpersandCollection = require('ampersand-collection'),
    UnderscoreMixin = require('ampersand-collection-underscore-mixin'),
    Stat = require('./stat');

var StatCollection = module.exports = AmpersandCollection.extend(UnderscoreMixin, {
  comparator: 'name',
  model: Stat
});
