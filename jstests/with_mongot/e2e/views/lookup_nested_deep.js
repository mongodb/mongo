/**
 * This test nests 4 $lookup stages within each other, all of which include $search queries
 * on separate views. The test verifies that the view definitions are applied correctly and the
 * results are as expected.
 *
 * @tags: [ requires_fcv_81, featureFlagMongotIndexedViews ]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());

const publications = testDb.publications;
const authors = testDb.authors;
const universities = testDb.universities;
const conferences = testDb.conferences;
const keywords = testDb.keywords;

publications.drop();
authors.drop();
universities.drop();
conferences.drop();
keywords.drop();

assert.commandWorked(publications.insertMany([
    {
        _id: 1,
        title: "Advances in Machine Learning Algorithms",
        year: 2020,
        author_ids: [101, 102],
        conference_id: 1,
        keyword_ids: [501, 502, 503]
    },
    {
        _id: 2,
        title: "Neural Networks for Natural Language Processing",
        year: 2019,
        author_ids: [102, 103],
        conference_id: 2,
        keyword_ids: [501, 504]
    },
    {
        _id: 3,
        title: "Quantum Computing Principles",
        year: 2021,
        author_ids: [101, 104],
        conference_id: 3,
        keyword_ids: [505, 506]
    },
    {
        _id: 4,
        title: "Blockchain Applications in Healthcare",
        year: 2018,
        author_ids: [103, 105],
        conference_id: 4,
        keyword_ids: [507, 508]
    },
    {
        _id: 5,
        title: "Deep Learning in Computer Vision",
        year: 2022,
        author_ids: [102, 104, 105],
        conference_id: 1,
        keyword_ids: [501, 502, 509]
    }
]));

assert.commandWorked(authors.insertMany([
    {
        _id: 101,
        name: "Dr. Jane Smith",
        university_id: 201,
        research_areas: ["AI", "Machine Learning", "Quantum Computing"]
    },
    {
        _id: 102,
        name: "Prof. John Davis",
        university_id: 202,
        research_areas: ["Deep Learning", "Natural Language Processing", "Computer Vision"]
    },
    {
        _id: 103,
        name: "Dr. Sarah Johnson",
        university_id: 203,
        research_areas: ["Natural Language Processing", "Blockchain", "Privacy"]
    },
    {
        _id: 104,
        name: "Prof. Michael Chen",
        university_id: 201,
        research_areas: ["Quantum Computing", "Computer Vision", "AI"]
    },
    {
        _id: 105,
        name: "Dr. Lisa Wong",
        university_id: 204,
        research_areas: ["Blockchain", "Healthcare Informatics", "Computer Vision"]
    }
]));

assert.commandWorked(universities.insertMany([
    {
        _id: 201,
        name: "Stanford University",
        location: "California, USA",
        departments: ["Computer Science", "Mathematics", "Physics"]
    },
    {
        _id: 202,
        name: "Massachusetts Institute of Technology",
        location: "Massachusetts, USA",
        departments: ["Computer Science", "Electrical Engineering", "Mathematics"]
    },
    {
        _id: 203,
        name: "University of Oxford",
        location: "Oxford, UK",
        departments: ["Computer Science", "Mathematics", "Philosophy"]
    },
    {
        _id: 204,
        name: "ETH Zurich",
        location: "Zurich, Switzerland",
        departments: ["Computer Science", "Physics", "Mathematics"]
    }
]));

assert.commandWorked(conferences.insertMany([
    {
        _id: 1,
        name: "International Conference on Machine Learning",
        acronym: "ICML",
        year: 2020,
        location: "Vienna, Austria"
    },
    {
        _id: 2,
        name: "Conference on Neural Information Processing Systems",
        acronym: "NeurIPS",
        year: 2019,
        location: "Vancouver, Canada"
    },
    {_id: 3, name: "IEEE Quantum Week", acronym: "QCE", year: 2021, location: "Broomfield, USA"},
    {
        _id: 4,
        name: "IEEE International Conference on Blockchain",
        acronym: "ICBC",
        year: 2018,
        location: "Halifax, Canada"
    }
]));

assert.commandWorked(keywords.insertMany([
    {_id: 501, term: "Machine Learning", category: "AI", papers_count: 4500},
    {_id: 502, term: "Deep Learning", category: "AI", papers_count: 3200},
    {_id: 503, term: "Algorithms", category: "Computer Science", papers_count: 7800},
    {_id: 504, term: "Natural Language Processing", category: "AI", papers_count: 2900},
    {_id: 505, term: "Quantum Computing", category: "Computer Science", papers_count: 1200},
    {_id: 506, term: "Quantum Algorithms", category: "Computer Science", papers_count: 850},
    {_id: 507, term: "Blockchain", category: "Computer Science", papers_count: 1800},
    {_id: 508, term: "Healthcare", category: "Domain Application", papers_count: 3600},
    {_id: 509, term: "Computer Vision", category: "AI", papers_count: 4100}
]));

// Create views on all collections.
const publicationsViewPipeline = [{
    $addFields: {
        formatted_title: {$concat: ["\"", "$title", "\" (", {$toString: "$year"}, ")"]},
        is_recent: {$gte: ["$year", 2020]}
    }
}];
assert.commandWorked(
    testDb.createView("publicationsView", publications.getName(), publicationsViewPipeline));
const publicationsView = testDb.publicationsView;

const authorsViewPipeline = [{
    $addFields: {
        display_name: {$concat: ["$name", " (", {$arrayElemAt: ["$research_areas", 0]}, ")"]},
        primary_area: {$arrayElemAt: ["$research_areas", 0]}
    }
}];
assert.commandWorked(testDb.createView("authorsView", authors.getName(), authorsViewPipeline));
const authorsView = testDb.authorsView;

const universitiesViewPipeline =
    [{$addFields: {full_name: {$concat: ["$name", " (", "$location", ")"]}}}];
assert.commandWorked(
    testDb.createView("universitiesView", universities.getName(), universitiesViewPipeline));
const universitiesView = testDb.universitiesView;

const conferencesViewPipeline =
    [{$addFields: {full_name: {$concat: ["$name", " (", "$acronym", ")"]}}}];
assert.commandWorked(
    testDb.createView("conferencesView", conferences.getName(), conferencesViewPipeline));
const conferencesView = testDb.conferencesView;

const keywordsViewPipeline = [{
    $addFields: {
        display_term: {$concat: ["$term", " (", "$category", ")"]},
        popularity: {
            $switch: {
                branches: [
                    {case: {$gte: ["$papers_count", 3000]}, then: "very-popular"},
                    {case: {$gte: ["$papers_count", 1000]}, then: "popular"}
                ],
                default: "specialized"
            }
        }
    }
}];
assert.commandWorked(testDb.createView("keywordsView", keywords.getName(), keywordsViewPipeline));
const keywordsView = testDb.keywordsView;

// Create search indexes on all views.
const publicationsIndexName = "publicationsIndex";
const authorsIndexName = "authorsIndex";
const universitiesIndexName = "universitiesIndex";
const conferencesIndexName = "conferencesIndex";
const keywordsIndexName = "keywordsIndex";
createSearchIndex(publicationsView,
                  {name: publicationsIndexName, definition: {mappings: {dynamic: true}}});
createSearchIndex(authorsView, {name: authorsIndexName, definition: {mappings: {dynamic: true}}});
createSearchIndex(universitiesView,
                  {name: universitiesIndexName, definition: {mappings: {dynamic: true}}});
createSearchIndex(conferencesView,
                  {name: conferencesIndexName, definition: {mappings: {dynamic: true}}});
createSearchIndex(keywordsView, {name: keywordsIndexName, definition: {mappings: {dynamic: true}}});

/**
 * This pipeline contains a deeply nested $lookup with search queries at each level:
 * 1. The top level searches for publications with "Machine Learning" in their title
 * 2. First $lookup finds authors who have "Deep Learning" in their research areas
 * 3. Second $lookup finds universities with "Computer Science" departments
 * 4. Third $lookup finds conferences related to "Machine Learning"
 * 5. Final $lookup finds "very-popular" keywords in the "AI" category
 *
 * The pipeline applies $match stages at each level to filter out results where inner lookups would
 * return empty arrays. This ensures we only get publications that:
 * - Contain "Machine Learning" in the title
 * - Have at least one author who researches "Deep Learning"
 * - That author works at a university with a "Computer Science" department
 * - That university is associated with conferences about "Machine Learning"
 * - Those conferences feature "very-popular" AI keywords
 *
 * The expected results are publications 1 ("Advances in Machine Learning Algorithms") and 5
 * ("Deep Learning in Computer Vision"), with their complete nested document structure.
 */
const pipeline = [
    {$search: {index: publicationsIndexName, text: {query: "Machine Learning", path: "title"}}},
    {
        $lookup: {
            from: authorsView.getName(),
            localField: "author_ids",
            foreignField: "_id",
            as: "authors_info",
            pipeline: [
                {
                    $search: {
                        index: authorsIndexName,
                        text: {
                            query: "Deep Learning",
                            path: "research_areas"
                        }
                    }
                },
                {
                    $lookup: {
                        from: universitiesView.getName(),
                        localField: "university_id",
                        foreignField: "_id",
                        as: "university_info",
                        pipeline: [
                            {
                                $search: {
                                    index: universitiesIndexName,
                                    text: {
                                        query: "Computer Science",
                                        path: "departments"
                                    }
                                }
                            },
                            {
                                $lookup: {
                                    from: conferencesView.getName(),
                                    pipeline: [
                                        {
                                            $search: {
                                                index: conferencesIndexName,
                                                text: {
                                                    query: "Machine Learning",
                                                    path: "name"
                                                }
                                            }
                                        },
                                        {
                                            $lookup: {
                                                from: keywordsView.getName(),
                                                pipeline: [
                                                    {
                                                        $search: {
                                                            index: keywordsIndexName,
                                                            text: {
                                                                query: "AI",
                                                                path: "category"
                                                            }
                                                        }
                                                    },
                                                    {
                                                        $match: {
                                                            popularity: "very-popular"
                                                        }
                                                    },
                                                    {$sort: {_id: 1}}
                                                ],
                                                as: "relevant_keywords"
                                            }
                                        },
                                        {
                                            $match: {
                                                "relevant_keywords": { $ne: [] }
                                            }
                                        }
                                    ],
                                    as: "related_conferences"
                                }
                            },
                            {
                                $match: {
                                    "related_conferences": { $ne: [] }
                                }
                            }
                        ]
                    }
                },
                {
                    $match: {
                        "university_info": { $ne: [] }
                    }
                },
                {$sort: {_id: 1}}
            ]
        }
    },
    {
        $match: {
            "authors_info": { $ne: [] }
        }
    },
    {$sort: {_id: 1}}
];

// Assert that the top-level view definition is applied correctly in the explain output.
const explain = assert.commandWorked(publicationsView.explain().aggregate(pipeline));
assertViewAppliedCorrectly(explain, pipeline, publicationsViewPipeline);

const results = publicationsView.aggregate(pipeline).toArray();
const expectedResults = [
    {
        _id: 1,
        title: "Advances in Machine Learning Algorithms",
        year: 2020,
        author_ids: [101, 102],
        conference_id: 1,
        keyword_ids: [501, 502, 503],
        formatted_title: "\"Advances in Machine Learning Algorithms\" (2020)",
        is_recent: true,
        authors_info: [
            {
                _id: 101,
                name: "Dr. Jane Smith",
                university_id: 201,
                research_areas: ["AI", "Machine Learning", "Quantum Computing"],
                display_name: "Dr. Jane Smith (AI)",
                primary_area: "AI",
                university_info: [{
                    _id: 201,
                    name: "Stanford University",
                    location: "California, USA",
                    departments: ["Computer Science", "Mathematics", "Physics"],
                    full_name: "Stanford University (California, USA)",
                    related_conferences: [{
                        _id: 1,
                        name: "International Conference on Machine Learning",
                        acronym: "ICML",
                        year: 2020,
                        location: "Vienna, Austria",
                        full_name: "International Conference on Machine Learning (ICML)",
                        relevant_keywords: [
                            {
                                _id: 501,
                                term: "Machine Learning",
                                category: "AI",
                                papers_count: 4500,
                                display_term: "Machine Learning (AI)",
                                popularity: "very-popular"
                            },
                            {
                                _id: 502,
                                term: "Deep Learning",
                                category: "AI",
                                papers_count: 3200,
                                display_term: "Deep Learning (AI)",
                                popularity: "very-popular"
                            },
                            {
                                _id: 509,
                                term: "Computer Vision",
                                category: "AI",
                                papers_count: 4100,
                                display_term: "Computer Vision (AI)",
                                popularity: "very-popular"
                            }
                        ]
                    }]
                }]
            },
            {
                _id: 102,
                name: "Prof. John Davis",
                university_id: 202,
                research_areas: ["Deep Learning", "Natural Language Processing", "Computer Vision"],
                display_name: "Prof. John Davis (Deep Learning)",
                primary_area: "Deep Learning",
                university_info: [{
                    _id: 202,
                    name: "Massachusetts Institute of Technology",
                    location: "Massachusetts, USA",
                    departments: ["Computer Science", "Electrical Engineering", "Mathematics"],
                    full_name: "Massachusetts Institute of Technology (Massachusetts, USA)",
                    related_conferences: [{
                        _id: 1,
                        name: "International Conference on Machine Learning",
                        acronym: "ICML",
                        year: 2020,
                        location: "Vienna, Austria",
                        full_name: "International Conference on Machine Learning (ICML)",
                        relevant_keywords: [
                            {
                                _id: 501,
                                term: "Machine Learning",
                                category: "AI",
                                papers_count: 4500,
                                display_term: "Machine Learning (AI)",
                                popularity: "very-popular"
                            },
                            {
                                _id: 502,
                                term: "Deep Learning",
                                category: "AI",
                                papers_count: 3200,
                                display_term: "Deep Learning (AI)",
                                popularity: "very-popular"
                            },
                            {
                                _id: 509,
                                term: "Computer Vision",
                                category: "AI",
                                papers_count: 4100,
                                display_term: "Computer Vision (AI)",
                                popularity: "very-popular"
                            }
                        ]
                    }]
                }]
            }
        ]
    },
    {
        _id: 5,
        title: "Deep Learning in Computer Vision",
        year: 2022,
        author_ids: [102, 104, 105],
        conference_id: 1,
        keyword_ids: [501, 502, 509],
        formatted_title: "\"Deep Learning in Computer Vision\" (2022)",
        is_recent: true,
        authors_info: [{
            _id: 102,
            name: "Prof. John Davis",
            university_id: 202,
            research_areas: ["Deep Learning", "Natural Language Processing", "Computer Vision"],
            display_name: "Prof. John Davis (Deep Learning)",
            primary_area: "Deep Learning",
            university_info: [{
                _id: 202,
                name: "Massachusetts Institute of Technology",
                location: "Massachusetts, USA",
                departments: ["Computer Science", "Electrical Engineering", "Mathematics"],
                full_name: "Massachusetts Institute of Technology (Massachusetts, USA)",
                related_conferences: [{
                    _id: 1,
                    name: "International Conference on Machine Learning",
                    acronym: "ICML",
                    year: 2020,
                    location: "Vienna, Austria",
                    full_name: "International Conference on Machine Learning (ICML)",
                    relevant_keywords: [
                        {
                            _id: 501,
                            term: "Machine Learning",
                            category: "AI",
                            papers_count: 4500,
                            display_term: "Machine Learning (AI)",
                            popularity: "very-popular"
                        },
                        {
                            _id: 502,
                            term: "Deep Learning",
                            category: "AI",
                            papers_count: 3200,
                            display_term: "Deep Learning (AI)",
                            popularity: "very-popular"
                        },
                        {
                            _id: 509,
                            term: "Computer Vision",
                            category: "AI",
                            papers_count: 4100,
                            display_term: "Computer Vision (AI)",
                            popularity: "very-popular"
                        }
                    ]
                }]
            }]
        }]
    }
];
assert.eq(expectedResults, results);

// Clean up search indexes.
dropSearchIndex(publicationsView, {name: publicationsIndexName});
dropSearchIndex(authorsView, {name: authorsIndexName});
dropSearchIndex(universitiesView, {name: universitiesIndexName});
dropSearchIndex(conferencesView, {name: conferencesIndexName});
dropSearchIndex(keywordsView, {name: keywordsIndexName});