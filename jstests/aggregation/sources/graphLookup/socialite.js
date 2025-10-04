// In MongoDB 3.4, $graphLookup was introduced. In this file, we test $graphLookup as applied to the
// Socialite schema example available here: https://github.com/mongodb-labs/socialite

let follower = db.followers;
let users = db.users;

follower.drop();
users.drop();

let userDocs = [
    {_id: "djw", fullname: "Darren", country: "Australia"},
    {_id: "bmw", fullname: "Bob", country: "Germany"},
    {_id: "jsr", fullname: "Jared", country: "USA"},
    {_id: "ftr", fullname: "Frank", country: "Canada"},
];

userDocs.forEach(function (userDoc) {
    assert.commandWorked(users.insert(userDoc));
});

let followers = [
    {_f: "djw", _t: "jsr"},
    {_f: "jsr", _t: "bmw"},
    {_f: "ftr", _t: "bmw"},
];

followers.forEach(function (f) {
    assert.commandWorked(follower.insert(f));
});

// Find the social network of "Darren", that is, people Darren follows, and people who are
// followed by someone Darren follows, etc.

let res = users
    .aggregate(
        {$match: {fullname: "Darren"}},
        {
            $graphLookup: {
                from: "followers",
                startWith: "$_id",
                connectFromField: "_t",
                connectToField: "_f",
                as: "network",
            },
        },
        {$unwind: "$network"},
        {$project: {_id: "$network._t"}},
    )
    .toArray();

// "djw" is followed, directly or indirectly, by "jsr" and "bmw".
assert.eq(res.length, 2);
