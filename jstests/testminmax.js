t = db.minmaxtest;
t.drop();
t.insert({"_id" : "IBM.N|00001264779918428889", "DESCRIPTION" : { "n" : "IBMSTK2", "o" : "IBM STK", "s" : "changed" } });
t.insert({ "_id" : "VOD.N|00001264779918433344", "COMPANYNAME" : { "n" :	"Vodafone Group PLC 2", "o" : "Vodafone Group PLC", "s" : "changed" } });
t.insert({ "_id" : "IBM.N|00001264779918437075", "DESCRIPTION" : { "n" : "IBMSTK3", "o" : "IBM STK2", "s" : "changed" } });
t.insert({ "_id" : "VOD.N|00001264779918441426", "COMPANYNAME" : { "n" :	"Vodafone Group PLC 3", "o" : "Vodafone Group PLC 2", "s" :	"changed" } });

// temp:
printjson( t.find().min({"_id":"IBM.N|00000000000000000000"}).max({"_id":"IBM.N|99999999999999999999"}).toArray() );

// this should be 2!!  add assertion when fixed
// http://jira.mongodb.org/browse/SERVER-675
print( t.find().min({"_id":"IBM.N|00000000000000000000"}).max({"_id":"IBM.N|99999999999999999999"}).count() );

