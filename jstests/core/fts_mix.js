
load("jstests/libs/fts.js");

// test collection
tc = db.text_mix;
tc.drop();

// creation of collection documents
// content generated using wikipedia random article
tc.save({
    _id: 1,
    title: "Olivia Shakespear",
    text:
        "Olivia Shakespear (born Olivia Tucker; 17 March 1863 – 3 October 1938) was a British novelist, playwright, and patron of the arts. She wrote six books that are described as \"marriage problem\" novels. Her works sold poorly, sometimes only a few hundred copies. Her last novel, Uncle Hilary, is considered her best. She wrote two plays in collaboration with Florence Farr."
});
tc.save({
    _id: 2,
    title: "Mahim Bora",
    text:
        "Mahim Bora (born 1926) is an Indian writer and educationist from Assam state. He was born at a tea estate of Sonitpur district. He is an M.A. in Assamese literature from Gauhati University and had been a teacher in the Nowgong College for most of his teaching career. He has now retired and lives at Nagaon. Bora spent a good part of his childhood in the culture-rich surroundings of rural Nagaon, where the river Kalong was the life-blood of a community. His impressionable mind was to capture a myriad memories of that childhood, later to find expression in his poems, short stories and novels with humour, irony and pathos woven into their texture. When this river was dammed up, its disturbing effect was on the entire community dependant on nature's bounty."
});
tc.save({
    _id: 3,
    title: "A break away!",
    text:
        "A break away! is an 1891 painting by Australian artist Tom Roberts. The painting depicts a mob of thirsty sheep stampeding towards a dam. A drover on horseback is attempting to turn the mob before they drown or crush each other in their desire to drink. The painting, an \"icon of Australian art\", is part of a series of works by Roberts that \"captures what was an emerging spirit of national identity.\" Roberts painted the work at Corowa. The painting depicts a time of drought, with little grass and the soil kicked up as dust. The work itself is a reflection on the pioneering days of the pastoral industry, which were coming to an end by the 1890s."
});
tc.save({
    _id: 4,
    title: "Linn-Kristin Riegelhuth Koren",
    text:
        "Linn-Kristin Riegelhuth Koren (born 1 August 1984, in Ski) is a Norwegian handballer playing for Larvik HK and the Norwegian national team. She is commonly known as Linka. Outside handball she is a qualified nurse."
});
tc.save({
    _id: 5,
    title: "Morten Jensen",
    text:
        "Morten Jensen (born December 2, 1982 in Lynge) is a Danish athlete. He primarily participates in long jump, 100 metres and 200 metres. He competed at the World Championships in 2005 and 2007, the 2006 World Indoor Championships, the 2006 European Championships, the 2007 World Championships and the 2008 Olympic Games without qualifying for the final round. He was runner-up in the 2010 Finnish Elite Games rankings, just missing out to Levern Spencer for that year's jackpot. He holds the Danish record in both long jump and 100 metres. He also holds the Danish indoor record in the 200 metres. He has been a part of the Sparta teamsine 2005, before then he was a part of FIF Hillerd. His coach was Leif Dahlberg after the 2010 European Championships he change to Lars Nielsen and Anders Miller."
});
tc.save({
    _id: 6,
    title: "Janet Laurence",
    text:
        "Janet Laurence (born 1947) is a Sydney based Australian artist who works in mixed media and installation. Her work has been included in major survey exhibitions, nationally and internationally and is regularly exhibited in Sydney, Melbourne and Japan. Her work explores a relationship to the natural world, often from an architectural context. It extends from the gallery space into the urban fabric, and has been realized in many site specific projects, often involving collaborations with architects, landscape architects and environmental scientists. She has received many grants and awards including a Rockefeller Residency in 1997. Laurence was a Trustee of the Art Gallery of NSW from 1995 to 2005. Laurence was the subject of John Beard's winning entry for the 2007 Archibald Prize."
});
tc.save({
    _id: 7,
    title: "Glen-Coats Baronets",
    text:
        "The Glen-Coats Baronetcy, of Ferguslie Park in the Parish of Abbey in the County of Renfrew, was a title in the Baronetage of the United Kingdom. It was created on 25 June 1894 for Thomas Glen-Coats, Director of the thread-making firm of J. & P. Coats, Ltd, and later Liberal Member of Parliament for Renfrewshire West. Born Thomas Coats, he assumed the additional surname of Glen, which was that of his maternal grandfather. He was succeeded by his son, the second Baronet. He won a gold medal in sailing at the 1908 Summer Olympics. The title became extinct on his death in 1954. Two other members of the Coats family also gained distinction. George Coats, 1st Baron Glentanar, was the younger brother of the first Baronet, while Sir James Coats, 1st Baronet (see Coats Baronets), was the first cousin of the first Baronet."
});
tc.save({
    _id: 8,
    title: "Grapeleaf Skeletonizer",
    text:
        "The Grapeleaf Skeletonizer, Harrisina americana is a moth in the family Zygaenidae. It is widespread in the eastern half of the United States, and commonly noticed defoliating grapes, especially of the Virginia creeper (Parthenocissus quinquefolia). The western grapeleaf skeletonizer, Harrisina brillians is very similar to and slightly larger than H. americana, but their distributions are different. Members of this family all produce hydrogen cyanide, a potent antipredator toxin."
});
tc.save({
    _id: 9,
    title: "Physics World",
    text:
        "Physics World is the membership magazine of the Institute of Physics, one of the largest physical societies in the world. It is an international monthly magazine covering all areas of physics, both pure and applied, and is aimed at physicists in research, industry and education worldwide. It was launched in 1988 by IOP Publishing Ltd and has established itself as one of the world's leading physics magazines. The magazine is sent free to members of the Institute of Physics, who can also access a digital edition of the magazine, although selected articles can be read by anyone for free online. It was redesigned in September 2005 and has an audited circulation of just under 35000. The current editor is Matin Durrani. Also on the team are Dens Milne (associate editor), Michael Banks (news editor), Louise Mayor (features editor) and Margaret Harris (reviews and careers editor). Hamish Johnston is the editor of the magazine's website physicsworld.com and James Dacey is its reporter."
});
tc.save({
    _id: 10,
    title: "Mallacoota, Victoria",
    text:
        "Mallacoota is a small town in the East Gippsland region of Victoria, Australia. At the 2006 census, Mallacoota had a population of 972. At holiday times, particularly Easter and Christmas, the population increases by about 8,000. It is one of the most isolated towns in the state of Victoria, 25 kilometres off the Princes Highway and 523 kilometres (325 mi) from Melbourne. It is 526 kilometres (327 mi) from Sydney, New South Wales. It is halfway between Melbourne and Sydney when travelling via Princes Highway, though that is a long route between Australia's two main cities. It is the last official township on Victoria's east coast before the border with New South Wales. Mallacoota has a regional airport (Mallacoota Airport) YMCO (XMC) consisting of a grassed field for private light planes. It is known for its wild flowers, abalone industry, the inlet estuary consisting of Top Lake and Bottom Lake, and Croajingolong National Park that surround it. It is a popular and beautiful holiday spot for boating, fishing, walking the wilderness coast, swimming, birdwatching, and surfing. The Mallacoota Arts Council runs events throughout each year. Mallacoota Inlet is one of the main villages along the wilderness coast walk from NSW to Victoria, Australia."
});

// begin tests

// -------------------------------------------- INDEXING & WEIGHTING -------------------------------

// start with basic index, one item with default weight
tc.ensureIndex({"title": "text"});

// test the single result case..
res = tc.find({"$text": {"$search": "Victoria"}});
assert.eq(1, res.length());
assert.eq(10, res[0]._id);

tc.dropIndexes();

// now let's see about multiple fields, with specific weighting
tc.ensureIndex({"title": "text", "text": "text"}, {weights: {"title": 10}});
assert.eq([9, 7, 8], queryIDS(tc, "members physics"));

tc.dropIndexes();

// test all-1 weighting with "$**"
tc.ensureIndex({"$**": "text"});
assert.eq([2, 8, 7], queryIDS(tc, "family tea estate"));

tc.dropIndexes();

// non-1 weight on "$**" + other weight specified for some field
tc.ensureIndex({"$**": "text"}, {weights: {"$**": 10, "text": 2}});
assert.eq([7, 5], queryIDS(tc, "Olympic Games gold medal"));

tc.dropIndexes();

// -------------------------------------------- "search"ING
// ------------------------------------------

// go back to "$**": 1, "title": 10.. and test more specific "search" functionality!
tc.ensureIndex({"$**": "text"}, {weights: {"title": 10}});

// -------------------------------------------- STEMMING -------------------------------------------

// tests stemming for basic plural case
res = tc.find({"$text": {"$search": "member"}});
res2 = tc.find({"$text": {"$search": "members"}});
assert.eq(getIDS(res), getIDS(res2));

// "search" for something with potential 's bug.
res = tc.find({"$text": {"$search": "magazine's"}});
res2 = tc.find({"$text": {"$search": "magazine"}});
assert.eq(getIDS(res), getIDS(res2));

// -------------------------------------------- LANGUAGE -------------------------------------------

assert.throws(tc.find({"$text": {"$search": "member", $language: "spanglish"}}));
assert.doesNotThrow(function() {
    tc.find({"$text": {"$search": "member", $language: "english"}});
});

// -------------------------------------------- LIMIT RESULTS --------------------------------------

// ensure limit limits results
assert.eq([2], queryIDS(tc, "rural river dam", null, null, 1));

// ensure top results are the same regardless of limit
// make sure that this uses a case where it wouldn't be otherwise..
res = tc.find({"$text": {"$search": "united kingdom british princes"}}).limit(1);
res2 = tc.find({"$text": {"$search": "united kingdom british princes"}});
assert.eq(1, res.length());
assert.eq(4, res2.length());
assert.eq(res[0]._id, res2[0]._id);

// -------------------------------------------- PROJECTION -----------------------------------------

// test projection.. show just title and id
res = tc.find({"$text": {"$search": "Morten Jensen"}}, {title: 1});
assert.eq(1, res.length());
assert.eq(5, res[0]._id);
assert.eq(null, res[0].text);
assert.neq(null, res[0].title);
assert.neq(null, res[0]._id);

// test negative projection, ie. show everything but text
res = tc.find({"$text": {"$search": "handball"}}, {text: 0});
assert.eq(1, res.length());
assert.eq(4, res[0]._id);
assert.eq(null, res[0].text);
assert.neq(null, res[0].title);
assert.neq(null, res[0]._id);

// test projection only title, no id
res = tc.find({"$text": {"$search": "Mahim Bora"}}, {_id: 0, title: 1});
assert.eq(1, res.length());
assert.eq("Mahim Bora", res[0].title);
assert.eq(null, res[0].text);
assert.neq(null, res[0].title);
assert.eq(null, res[0]._id);

// -------------------------------------------- NEGATION -------------------------------------------

// test negation
assert.eq([8], queryIDS(tc, "United -Kingdom"));
assert.eq(-1, tc.findOne({_id: 8}).text.search(/Kingdom/i));

// test negation edge cases... hyphens, double dash, etc.
assert.eq([4], queryIDS(tc, "Linn-Kristin"));

// -------------------------------------------- PHRASE MATCHING ------------------------------------

// test exact phrase matching on
assert.eq([7], queryIDS(tc, "\"Summer Olympics\""));
assert.neq(-1, tc.findOne({_id: 7}).text.indexOf("Summer Olympics"));

// phrasematch with other stuff.. negation, other terms, etc.
assert.eq([10], queryIDS(tc, "\"wild flowers\" Sydney"));

assert.eq([3], queryIDS(tc, "\"industry\" -Melbourne -Physics"));

// -------------------------------------------- EDGE CASES -----------------------------------------

// test empty string
res = tc.find({"$text": {"$search": ""}});
assert.eq(0, res.length());

// test string with a space in it
res = tc.find({"$text": {"$search": " "}});
assert.eq(0, res.length());

// -------------------------------------------- FILTERING ------------------------------------------

assert.eq([2], queryIDS(tc, "Mahim"));
assert.eq([2], queryIDS(tc, "Mahim", {_id: 2}));
assert.eq([], queryIDS(tc, "Mahim", {_id: 1}));
assert.eq([], queryIDS(tc, "Mahim", {_id: {$gte: 4}}));
assert.eq([2], queryIDS(tc, "Mahim", {_id: {$lte: 4}}));

// using regex conditional filtering
assert.eq([9], queryIDS(tc, "members", {title: {$regex: /Phy.*/i}}));

// -------------------------------------------------------------------------------------------------

assert(tc.validate().valid);
