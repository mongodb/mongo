# wtstats template

This subfolder contains the code to generate the `wtstats.html.template` file needed by the Python script `wtstats.py`, which is located in the `./tools` directory. 

The template is generated as a node.js "single page" application using several open source frameworks, see Dependencies below. The `wtstats.py` Python script parses stats files, transforms the data and inserts it as JSON into the template. The resulting HTML page can be openend in the browser.

### Build Process

To build the template, follow these steps: 

1. change into the `./tools/template` directory
2. run `npm install` to install all missing dependencies. 
3. run `npm run build` to build the template, pack everything into a single HTML file and copy it to its parent folder as `./tools/wtstats.html.template`. 

### Usage

Call the `wtstats.py` Python script with a stats file as argument, e.g.

```
python wtstats.py /path/to/my/WiredTigerStat.19.16
```

The script will create `wtstats.html` in the working directory, which can be viewed with any modern browser (latest versions of Chrome, Safari, FireFox, Internet Explorer supported).

For more information about _wtstats_ usage, check out the [wtstats documentation](https://github.com/wiredtiger/wiredtiger/wiki/WiredTiger-statistics) on the WiredTiger wiki.

### Dependencies

- [ampersand.js](http://ampersandjs.com/), MV* framework, several modules, all under [MIT license][1]
- [Bootstrap](http://www.getbootstrap.com/), page layout and UI, [MIT license][2]
- [d3](http://d3js.org/), data bindings and visualization, [BSD license][3]
- [jQuery](http://jquery.com/), DOM manipulation, [MIT license][4]
- [lodash](https://lodash.com/), object/array transformation, [MIT license][5]
- [fontawesome](http://fontawesome.io/), icons, [OFL and MIT licenses][6]


[1]: https://github.com/AmpersandJS/ampersand-view/blob/master/LICENSE.md
[2]: https://github.com/twbs/bootstrap/blob/master/LICENSE
[3]: https://github.com/mbostock/d3/blob/master/LICENSE
[4]: https://github.com/jquery/jquery/blob/2.1.3/MIT-LICENSE.txt
[5]: https://github.com/lodash/lodash/blob/master/LICENSE.txt
[6]: http://fontawesome.io/license/

