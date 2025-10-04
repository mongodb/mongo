/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman
    Copyright (c) 2011 Thomas Bernard

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#define FUSION_MAX_VECTOR_SIZE 50 
#define BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS 
#define BOOST_MPL_LIMIT_LIST_SIZE 50 
#define BOOST_MPL_LIMIT_VECTOR_SIZE 50

#include "../measure.hpp"
#include <boost/spirit/include/qi.hpp>
#include <boost/phoenix/core.hpp>
#include <boost/phoenix/operator.hpp>
#include <boost/phoenix/object.hpp>
#include <boost/phoenix/fusion.hpp>
#include <boost/phoenix/stl/container.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/io.hpp>
#include <boost/spirit/include/qi_permutation.hpp>
#include <boost/spirit/home/qi/string/tst_map.hpp>
#include <boost/spirit/repository/include/qi_kwd.hpp>
#include <boost/spirit/repository/include/qi_keywords.hpp>
#include <boost/optional.hpp>
#include <boost/spirit/home/phoenix/core/argument.hpp>
#include <boost/spirit/home/phoenix/bind/bind_member_variable.hpp>

#include <iostream>
#include <string>
#include <complex>
#include <vector>
#include <iterator>
#include <stdexcept>

#include <boost/preprocessor/control/if.hpp>
#include <boost/preprocessor/seq/for_each_i.hpp>
#include <boost/preprocessor/seq/size.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>

#define KEYS_5

#include "keywords.hpp"

#define declOptions(r, data, i, elem) boost::optional<int> BOOST_PP_CAT(option,i);
#define fusionOptions(r, data, i, elem) (boost::optional<int>, BOOST_PP_CAT(option,i))


namespace client
{
        namespace qi = boost::spirit::qi;
        namespace ascii = boost::spirit::ascii;

        ///////////////////////////////////////////////////////////////////////////
        //  Our parsedData struct
        ///////////////////////////////////////////////////////////////////////////
        //[tutorial_parsedData_struct
        struct parsedDataOptions
        {   
                BOOST_PP_SEQ_FOR_EACH_I(declOptions,_,keys)
        };
        struct parsedData
        {

                std::string name;
                parsedDataOptions options;        
                void clear()
                {
                        name.clear();            
                }
        };

        struct parsedData2
        {
                std::string name;
                BOOST_PP_SEQ_FOR_EACH_I(declOptions,_,keys)

                        void clear()
                        {
                                name.clear();            
                        }
        };
}

std::ostream &operator<<(std::ostream & os, client::parsedData &data)
{
        os << data.name <<std::endl;

#define generateOutput1(r, d, i, elem) if( BOOST_PP_CAT(data.options.option, i) ) os<< BOOST_PP_STRINGIZE( BOOST_PP_CAT(option,i)) <<" "<< * BOOST_PP_CAT(data.options.option , i)<<std::endl;
        BOOST_PP_SEQ_FOR_EACH_I(generateOutput1,_,keys)

                os<<std::endl;

        return os;
}

std::ostream &operator<<(std::ostream & os, client::parsedData2 &data)
{
        os << data.name <<std::endl;

#define generateOutput2(r, d, i, elem) if(BOOST_PP_CAT(data.option, i)) os<< BOOST_PP_STRINGIZE( BOOST_PP_CAT(option,i)) <<" "<< * BOOST_PP_CAT(data.option,i)<<std::endl;
        BOOST_PP_SEQ_FOR_EACH_I(generateOutput2,_,keys)

                os<<std::endl;

        return os;
}



BOOST_FUSION_ADAPT_STRUCT(
                client::parsedDataOptions,
                BOOST_PP_SEQ_FOR_EACH_I(fusionOptions,_,keys)
                )

BOOST_FUSION_ADAPT_STRUCT(
                client::parsedData,
                (std::string, name)
                (client::parsedDataOptions, options)    
                )

BOOST_FUSION_ADAPT_STRUCT(
                client::parsedData2,
                (std::string, name)
                BOOST_PP_SEQ_FOR_EACH_I(fusionOptions,_,keys)
                )
enum variation
{
        full,
        no_assign,
        assign
};
namespace client
{


        ///////////////////////////////////////////////////////////////////////////////
        //  Our parsedData parser
        ///////////////////////////////////////////////////////////////////////////////
        //[tutorial_parsedData_parser
        template <typename Iterator>
                struct permutation_parser : qi::grammar<Iterator, parsedData(), ascii::space_type>
        {
                permutation_parser() : permutation_parser::base_type(start)
                {
                        using qi::int_;
                        using qi::lit;
                        using qi::double_;
                        using qi::lexeme;
                        using ascii::char_;
                        using boost::phoenix::at_c;
                        using boost::phoenix::assign;
                        using qi::_r1;
                        using qi::_1;
                        using qi::_val;
                        using qi::omit;
                        using qi::repeat;


                        quoted_string %= lexeme[+(char_-' ')];

#define generateOptions1(r, data, i, elem) BOOST_PP_IF(i, ^(lit(elem) > int_) , (lit(elem) > int_))
                        options = (BOOST_PP_SEQ_FOR_EACH_I(generateOptions1,_,keys));

                        start %=
                                quoted_string 
                                >> options;                
                        ;
                        v_vals = repeat(1,2)[int_];
                }

                typedef parsedData parser_target_type;

                qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
                qi::rule<Iterator, parsedDataOptions(), ascii::space_type> options;
                qi::rule<Iterator, std::vector<int>(), ascii::space_type> v_vals;

                qi::rule<Iterator, parsedData(), ascii::space_type> start;
        };

        template <typename Iterator>
                struct alternative_parser : qi::grammar<Iterator, parsedData2(), ascii::space_type>
        {
                alternative_parser() : alternative_parser::base_type(start)
                {
                        using qi::int_;
                        using qi::lit;
                        using qi::double_;
                        using qi::lexeme;
                        using ascii::char_;
                        using boost::phoenix::at_c;                        
                        using qi::_r1;
                        using qi::_1;
                        using qi::_val;

                        quoted_string %= lexeme[+(char_-' ')];

#define generateOptions2(r, data, i, elem) BOOST_PP_IF(i, |(lit(elem) > int_[at_c<i+1>(_r1)=_1]) , (lit(elem) > int_[at_c<i+1>(_r1)=_1]))
                        options = (BOOST_PP_SEQ_FOR_EACH_I(generateOptions2,_,keys));

                        start =
                                quoted_string [at_c<0>(_val)=_1]
                                >> *options(_val);                
                        ;
                }

                typedef parsedData2 parser_target_type;

                qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
                qi::rule<Iterator, void(parsedData2 & ), ascii::space_type> options;
                qi::rule<Iterator, parsedData2(), ascii::space_type> start;
        };



        template <typename Iterator,typename variation>
                struct tst_parser : qi::grammar<Iterator, parsedData2(), ascii::space_type>
        {
                typedef variation variation_type;

                tst_parser() : tst_parser::base_type(startalias)
                {
                        namespace phx = boost::phoenix;
                        using qi::int_;
                        using qi::lit;
                        using qi::double_;
                        using qi::lexeme;
                        using ascii::char_;            
                        using boost::phoenix::at_c;                        
                        using qi::_r1;
                        using qi::_1;
                        using qi::_a;
                        using qi::_val;
                        using qi::locals;
                        using qi::parameterized_nonterminal;

                        startalias = start.alias();
                        quoted_string %= lexeme[+(char_-' ')];

#define generateRules(r, data, i, elem) BOOST_PP_CAT(rule,i) = int_[phx::at_c<i+1>(*phx::ref(currentObj))=_1];
                        BOOST_PP_SEQ_FOR_EACH_I(generateRules,_,keys)

#define generateOptions3(r, data, i, elem) (elem,& BOOST_PP_CAT(rule,i))


                                options.add BOOST_PP_SEQ_FOR_EACH_I(generateOptions3,_,keys);
                        switch(variation_type::value)
                        {
                                case full:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options [_a=_1] >> lazy(*_a));
                                                ;
                                                break;
                                        }
                                case no_assign:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options >> int_);
                                                ;
                                                break;
                                        }
                                case assign:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options [_a=_1] >> int_);
                                                ;
                                                break;
                                        }
                        }


                }

                parsedData2 *currentObj;

                typedef parsedData2 parser_target_type;

                qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
                typedef qi::rule<Iterator, ascii::space_type> optionsRule;
#define declareRules(r, data, i, elem) optionsRule BOOST_PP_CAT(rule,i);

                BOOST_PP_SEQ_FOR_EACH_I(declareRules,_,keys)

                        qi::symbols<char,optionsRule* > options;
                qi::rule<Iterator, parsedData2(), ascii::space_type> startalias;
                qi::rule<Iterator, parsedData2(), qi::locals<optionsRule*>, ascii::space_type> start;
        };



        template <typename Iterator,typename variation>
                struct tst_map_parser : qi::grammar<Iterator, parsedData2(), ascii::space_type>
        {
                typedef variation variation_type;
                tst_map_parser() : tst_map_parser::base_type(startalias)
                {
                        namespace phx = boost::phoenix;
                        using qi::int_;
                        using qi::lit;
                        using qi::double_;
                        using qi::lexeme;
                        using ascii::char_;            
                        using boost::phoenix::at_c;                        
                        using qi::_r1;
                        using qi::_1;
                        using qi::_a;
                        using qi::_val;
                        using qi::locals;
                        using qi::parameterized_nonterminal;

                        startalias = start.alias();
                        quoted_string %= lexeme[+(char_-' ')];

#define generateRules3(r, data, i, elem) BOOST_PP_CAT(rule,i) = int_[phx::at_c<i+1>(*phx::ref(currentObj))=_1];
                        BOOST_PP_SEQ_FOR_EACH_I(generateRules3,_,keys)

#define generateOptions3(r, data, i, elem) (elem,& BOOST_PP_CAT(rule,i))


                                options.add BOOST_PP_SEQ_FOR_EACH_I(generateOptions3,_,keys);

                        switch(variation_type::value)
                        {
                                case full:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options [_a=_1] >> lazy(*_a));
                                                ;
                                                break;
                                        }
                                case no_assign:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options >> int_);
                                                ;
                                                break;
                                        }
                                case assign:
                                        {
                                                start =
                                                        quoted_string [at_c<0>(_val)=_1][phx::ref(currentObj)=&_val]
                                                        >> *( options [_a=_1] >> int_);
                                                ;
                                                break;
                                        }
                        }
                }

                parsedData2 *currentObj;

                typedef parsedData2 parser_target_type;

                qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
                typedef qi::rule<Iterator, ascii::space_type> optionsRule;
#define declareRules(r, data, i, elem) optionsRule BOOST_PP_CAT(rule,i);

                BOOST_PP_SEQ_FOR_EACH_I(declareRules,_,keys)

                        qi::symbols<char,optionsRule*, boost::spirit::qi::tst_map<char,optionsRule*> > options;
                qi::rule<Iterator, parsedData2(), ascii::space_type> startalias;
                qi::rule<Iterator, parsedData2(), qi::locals<optionsRule*>, ascii::space_type> start;
        };


        template <typename Iterator>
                struct kwd_parser : qi::grammar<Iterator, parsedData(), ascii::space_type>
        {
                kwd_parser() : kwd_parser::base_type(start)
                {
                        using qi::int_;
                        using qi::lit;
                        using qi::double_;
                        using qi::lexeme;
                        using ascii::char_;
                        using qi::_r1;
                        using qi::_1;
                        using qi::_val;
                        using boost::spirit::repository::qi::kwd;

                        quoted_string %= lexeme[+(char_-' ')];

#define generateOptions4(r, data, i, elem) BOOST_PP_IF(i, / kwd( elem )[ int_ ] , kwd(  elem )[ int_ ] )
                        options = (BOOST_PP_SEQ_FOR_EACH_I(generateOptions4,_,keys));

                        start %=
                                quoted_string 
                                >> options;                
                        ;
                }

                typedef parsedData parser_target_type;

                qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
                qi::rule<Iterator, parsedDataOptions(), ascii::space_type> options;
                qi::rule<Iterator, boost::fusion::vector<boost::optional<int>,boost::optional<int> > () , ascii::space_type> v_vals;

                qi::rule<Iterator, parsedData(), ascii::space_type> start;
        };

}


template <typename parserType>
        struct timeParser : test::base{
                timeParser(const std::string & str) : str(str)
                {
                }
                parserType &get_parser(){
                        static parserType parser;
                        return parser;
                }    

                std::string str;

                void benchmark()    
                {

                        using boost::spirit::ascii::space;
                        bool r = false;
                        std::string::const_iterator end = str.end();
                        std::string::const_iterator iter = str.begin();


                        typename parserType::parser_target_type data;
                        r = phrase_parse(iter, end, get_parser(), space, data);

                        if (r && iter == end)
                        {
                                this->val += data.name.size();
                        }
                        else
                        {
                                throw std::runtime_error("Parsing failed");
                        }
                }

        };




typedef std::string::const_iterator iterator_type;
typedef client::permutation_parser<iterator_type> permutation_parser;
typedef client::kwd_parser<iterator_type> kwd_parser;
typedef client::alternative_parser<iterator_type> alternative_parser;
typedef client::tst_map_parser<iterator_type, boost::mpl::int_<full> > tst_map_parser;

struct permutation_timer_fwd : timeParser<permutation_parser>
{
        permutation_timer_fwd() : timeParser<permutation_parser>(fwd) {}
};

struct permutation_timer_back : timeParser<permutation_parser>
{
        permutation_timer_back() : timeParser<permutation_parser>(back) {}
};

struct alternative_timer_fwd : timeParser<alternative_parser>
{
        alternative_timer_fwd() : timeParser<alternative_parser>(fwd) {}
};

struct alternative_timer_back : timeParser<alternative_parser>
{
        alternative_timer_back() : timeParser<alternative_parser>(back) {}
};

struct tst_timer_fwd_full : timeParser< client::tst_parser<iterator_type, boost::mpl::int_<full> > >
{
        tst_timer_fwd_full() : timeParser< client::tst_parser<iterator_type, boost::mpl::int_<full> > >(fwd) {}
};

struct tst_timer_fwd_no_assign : timeParser< client::tst_parser<iterator_type, boost::mpl::int_<no_assign> > >
{
        tst_timer_fwd_no_assign() : timeParser< client::tst_parser<iterator_type,boost::mpl::int_<no_assign> > >(fwd) {}
};

struct tst_timer_fwd_assign : timeParser< client::tst_parser<iterator_type,boost::mpl::int_<assign> > >
{
        tst_timer_fwd_assign() : timeParser< client::tst_parser<iterator_type,boost::mpl::int_<assign> > >(fwd) {}
};



struct tst_timer_back : timeParser< client::tst_parser<iterator_type,boost::mpl::int_<full> > >
{
        tst_timer_back() : timeParser< client::tst_parser<iterator_type,boost::mpl::int_<full> > >(back) {}
};

struct tst_map_timer_fwd : timeParser<tst_map_parser>
{
        tst_map_timer_fwd() : timeParser<tst_map_parser>(fwd) {}
};

struct tst_map_timer_back : timeParser<tst_map_parser>
{
        tst_map_timer_back() : timeParser<tst_map_parser>(back) {}
};

struct kwd_timer_fwd : timeParser<kwd_parser>
{
        kwd_timer_fwd() : timeParser<kwd_parser>(fwd) {}
};

struct kwd_timer_back : timeParser<kwd_parser>
{
        kwd_timer_back() : timeParser<kwd_parser>(back) {}
};





////////////////////////////////////////////////////////////////////////////
//  Main program
////////////////////////////////////////////////////////////////////////////
        int
main()
{

        BOOST_SPIRIT_TEST_BENCHMARK(
                        10000000000,     // This is the maximum repetitions to execute
                        (permutation_timer_fwd)
                        (permutation_timer_back)
                        (alternative_timer_fwd)
                        (alternative_timer_back)
                        (tst_timer_fwd_full)
                        (tst_timer_fwd_no_assign)
                        (tst_timer_fwd_assign)
                        (tst_timer_back)
                        (tst_map_timer_fwd)
                        (tst_map_timer_back)
                        (kwd_timer_fwd)
                        (kwd_timer_back)
                        )

                // This is ultimately responsible for preventing all the test code
                // from being optimized away.  Change this to return 0 and you
                // unplug the whole test's life support system.
                return test::live_code != 0;
}


