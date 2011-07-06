/* sample articles for aggregation demonstrations */

// make sure we're using the right db; this is the same as "use mydb;" in shell
db = db.getSisterDB("aggdb");
db.article.drop();

db.article.save( {
    title : "this is my title" , 
    author : "bob" , 
    posted : new Date(1079895594000) , 
    pageViews : 5 , 
    tags : [ "fun" , "good" , "fun" ] ,
    comments : [ 
        { author :"joe" , text : "this is cool" } , 
        { author :"sam" , text : "this is bad" } 
    ],
    other : { foo : 5 }
});

db.article.save( {
    title : "this is your title" , 
    author : "dave" , 
    posted : new Date(4121381470000) , 
    pageViews : 7 , 
    tags : [ "fun" , "nasty" ] ,
    comments : [ 
        { author :"barbarella" , text : "this is hot" } , 
        { author :"leia" , text : "i prefer the brass bikini", votes: 10 } 
    ],
    other : { bar : 14 }
});

db.article.save( {
    title : "this is some other title" , 
    author : "jane" , 
    posted : new Date(978239834000) , 
    pageViews : 6 , 
    tags : [ "nasty" , "filthy" ] ,
    comments : [ 
        { author :"r2" , text : "beep boop" } , 
        { author :"leia" , text : "this is too smutty" } 
    ],
    other : { bar : 14 }
});
