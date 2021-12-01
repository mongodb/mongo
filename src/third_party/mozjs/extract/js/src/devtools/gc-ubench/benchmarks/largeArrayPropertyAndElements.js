window.tests.set('largeArrayPropertyAndElements', (function() {
  var garbage;
  var index;

  return {
    description: "Large array with both properties and elements",

    load: n => {
      garbage = new Array(n);
      garbage.fill(null);
      index = 0;
    },

    unload: () => {
      garbage = null;
      index = 0;
    },

    defaultGarbageTotal: "100K",
    defaultGarbagePerFrame: "30K",

    makeGarbage: n => {
      for (var i = 0; i < n; i++) {
        index++;
        index %= garbage.length;

        var obj = {};
        garbage[index] = obj;
        garbage["key-" + index] = obj;
      }
    }
  };

}()));
