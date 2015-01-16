var d3 = require('d3');

var ColorSingleton = module.exports = (function () {
    var instance;
 
    function createInstance() {
        var object = d3.scale.category20();
        return object;
    }
 
    return {
        getInstance: function () {
            if (!instance) {
                instance = createInstance();
            }
            return instance;
        }
    };
})();
