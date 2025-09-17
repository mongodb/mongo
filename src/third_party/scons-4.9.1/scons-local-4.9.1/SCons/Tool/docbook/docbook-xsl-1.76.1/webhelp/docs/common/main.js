/**
 * Miscellaneous js functions for WebHelp
 * Kasun Gajasinghe, http://kasunbg.blogspot.com
 * David Cramer, http://www.thingbag.net
 *
 */

$(document).ready(function() {  
  //  $("#showHideHighlight").button(); //add jquery button styling to 'Go' button
    //Generate tabs in nav-pane with JQuery
    $(function() {
            $("#tabs").tabs({
                cookie: {
                    // store cookie for 2 days.
                    expires: 2
                }
            });
        });

    //Generate the tree
     $("#ulTreeDiv").attr("style","");
    $("#tree").treeview({
        collapsed: true,
        animated: "medium",
        control: "#sidetreecontrol",
        persist: "cookie"
    });

    //after toc fully styled, display it. Until loading, a 'loading' image will be displayed
    $("#tocLoading").attr("style","display:none;");
//    $("#ulTreeDiv").attr("style","display:block;");

    //.searchButton is the css class applied to 'Go' button 
    $(function() {
		$("button", ".searchButton").button();

		$("button", ".searchButton").click(function() { return false; });
	});

    //'ui-tabs-1' is the cookie name which is used for the persistence of the tabs.(Content/Search tab)
    if ($.cookie('ui-tabs-1') === '1') {    //search tab is visible 
        if ($.cookie('textToSearch') != undefined && $.cookie('textToSearch').length > 0) {
            document.getElementById('textToSearch').value = $.cookie('textToSearch');
            Verifie('diaSearch_Form');
            searchHighlight($.cookie('textToSearch'));
            $("#showHideHighlight").css("display","block");
        }
    }

    syncToc(); //Synchronize the toc tree with the content pane, when loading the page.
    //$("#doSearch").button(); //add jquery button styling to 'Go' button
});

/**
 * Synchronize with the tableOfContents 
 */
function syncToc(){
    var a = document.getElementById("webhelp-currentid");
    if (a != undefined) {
        var b = a.getElementsByTagName("a")[0];

        if (b != undefined) {
            //Setting the background for selected node.
            var style = a.getAttribute("style");
            if (style != null && !style.match(/background-color: Background;/)) {
                a.setAttribute("style", "background-color: #6495ed;  " + style);
                b.setAttribute("style", "color: white;");
            } else if (style != null) {
                a.setAttribute("style", "background-color: #6495ed;  " + style);
                b.setAttribute("style", "color: white;");
            } else {
                a.setAttribute("style", "background-color: #6495ed;  ");
                b.setAttribute("style", "color: white;");
            }
        }

        //shows the node related to current content.
        //goes a recursive call from current node to ancestor nodes, displaying all of them.
        while (a.parentNode && a.parentNode.nodeName) {
            var parentNode = a.parentNode;
            var nodeName = parentNode.nodeName;

            if (nodeName.toLowerCase() == "ul") {
                parentNode.setAttribute("style", "display: block;");
            } else if (nodeName.toLocaleLowerCase() == "li") {
                parentNode.setAttribute("class", "collapsable");
                parentNode.firstChild.setAttribute("class", "hitarea collapsable-hitarea ");
            }
            a = parentNode;
        }
    }
}

/**
 * Code for Show/Hide TOC
 *
 */
function showHideToc() {
    var showHideButton = $("#showHideButton");
    var leftNavigation = $("#leftnavigation");
    var content = $("#content");

    if (showHideButton != undefined && showHideButton.hasClass("pointLeft")) {
        //Hide TOC
        showHideButton.removeClass('pointLeft').addClass('pointRight');
        content.css("margin", "0 0 0 0");
        leftNavigation.css("display","none");
        showHideButton.attr("title", "Show the TOC tree");
    } else {
        //Show the TOC
        showHideButton.removeClass('pointRight').addClass('pointLeft');
        content.css("margin", "0 0 0 280px");
        leftNavigation.css("display","block");
        showHideButton.attr("title", "Hide the TOC Tree");
    }
}

/**
 * Code for searh highlighting
 */
var highlightOn = true;
function searchHighlight(searchText) {
    highlightOn = true;
    if (searchText != undefined) {
        var wList;
        var sList = new Array();    //stem list 
        //Highlight the search terms
        searchText = searchText.toLowerCase().replace(/<\//g, "_st_").replace(/\$_/g, "_di_").replace(/\.|%2C|%3B|%21|%3A|@|\/|\*/g, " ").replace(/(%20)+/g, " ").replace(/_st_/g, "</").replace(/_di_/g, "%24_")
        searchText = searchText.replace(/  +/g, " ");
        searchText = searchText.replace(/ $/, "").replace(/^ /, "");

        wList = searchText.split(" ");
        $("#content").highlight(wList); //Highlight the search input

        if(typeof stemmer != "undefined" ){
            //Highlight the stems
            for (var i = 0; i < wList.length; i++) {
                var stemW = stemmer(wList[i]);
                sList.push(stemW);
            }
        } else {
            sList = wList;
        }
        $("#content").highlight(sList); //Highlight the search input's all stems
    } 
}

function searchUnhighlight(){
    highlightOn = false;
     //unhighlight the search input's all stems
    $("#content").unhighlight();
    $("#content").unhighlight();
}

function toggleHighlight(){
    if(highlightOn) {
        searchUnhighlight();
    } else {
        searchHighlight($.cookie('textToSearch'));
    }
}