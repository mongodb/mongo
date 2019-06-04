//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#include "mo_lambda.hpp"
#include <string.h>
#include <stdlib.h>

namespace boost {
namespace locale {
namespace gnu_gettext {
namespace lambda {

namespace { // anon
    struct identity : public plural {
        virtual int operator()(int n) const 
        {
            return n;
        };
        virtual identity *clone() const
        {
            return new identity();
        }
    };

    struct unary : public plural 
    {
        unary(plural_ptr ptr) : 
            op1(ptr)
        {
        }
    protected:
        plural_ptr op1;
    };


    struct binary : public plural 
    {
        binary(plural_ptr p1,plural_ptr p2) :
            op1(p1),
            op2(p2)
        {
        }
    protected:
        plural_ptr op1,op2;
    };

    struct number : public plural
    {
        number(int v) : 
            val(v)
        {
        }
        virtual int operator()(int /*n*/) const 
        {
            return val;
        }
        virtual number *clone() const
        {
            return new number(val);
        }

    private:
        int val;
    };

    #define UNOP(name,oper)                         \
    struct name: public unary {                     \
        name(plural_ptr op) : unary(op)             \
        {                                           \
        };                                          \
        virtual int operator()(int n) const         \
        {                                           \
            return oper (*op1)(n);                  \
        }                                           \
        virtual name *clone() const                 \
        {                                           \
            plural_ptr op1_copy(op1->clone());      \
            return new name(op1_copy);              \
        }                                           \
    };

    #define BINOP(name,oper)                        \
    struct name : public binary                     \
    {                                               \
        name(plural_ptr p1,plural_ptr p2) :         \
            binary(p1,p2)                           \
        {                                           \
        }                                           \
                                                    \
        virtual int operator()(int n) const         \
        {                                           \
            return (*op1)(n) oper (*op2)(n);        \
        }                                           \
        virtual name *clone() const                 \
        {                                           \
            plural_ptr op1_copy(op1->clone());      \
            plural_ptr op2_copy(op2->clone());      \
            return new name(op1_copy,op2_copy);     \
        }                                           \
    };

    #define BINOPD(name,oper)                       \
    struct name : public binary {                   \
        name(plural_ptr p1,plural_ptr p2) :         \
            binary(p1,p2)                           \
        {                                           \
        }                                           \
        virtual int operator()(int n) const         \
        {                                           \
            int v1=(*op1)(n);                       \
            int v2=(*op2)(n);                       \
            return v2==0 ? 0 : v1 oper v2;          \
        }                                           \
        virtual name *clone() const                 \
        {                                           \
            plural_ptr op1_copy(op1->clone());      \
            plural_ptr op2_copy(op2->clone());      \
            return new name(op1_copy,op2_copy);     \
        }                                           \
    };

    enum { END = 0 , SHL = 256,  SHR, GTE,LTE, EQ, NEQ, AND, OR, NUM, VARIABLE };

    UNOP(l_not,!)
    UNOP(minus,-)
    UNOP(bin_not,~)

    BINOP(mul,*)
    BINOPD(div,/)
    BINOPD(mod,%)
    static int level10[]={3,'*','/','%'};

    BINOP(add,+)
    BINOP(sub,-)
    static int level9[]={2,'+','-'};

    BINOP(shl,<<)
    BINOP(shr,>>)
    static int level8[]={2,SHL,SHR};

    BINOP(gt,>)
    BINOP(lt,<)
    BINOP(gte,>=)
    BINOP(lte,<=)
    static int level7[]={4,'<','>',GTE,LTE};

    BINOP(eq,==)
    BINOP(neq,!=)
    static int level6[]={2,EQ,NEQ};

    BINOP(bin_and,&)
    static int level5[]={1,'&'};

    BINOP(bin_xor,^)
    static int level4[]={1,'^'};

    BINOP(bin_or,|)
    static int level3[]={1,'|'};

    BINOP(l_and,&&)
    static int level2[]={1,AND};

    BINOP(l_or,||)
    static int level1[]={1,OR};

    struct conditional : public plural {
        conditional(plural_ptr p1,plural_ptr p2,plural_ptr p3) :
             op1(p1),
             op2(p2),
             op3(p3)
        {
        }
        virtual int operator()(int n) const
        {
            return (*op1)(n) ? (*op2)(n) : (*op3)(n);
        }
        virtual conditional *clone() const                 
        {                                           
            plural_ptr op1_copy(op1->clone());      
            plural_ptr op2_copy(op2->clone());      
            plural_ptr op3_copy(op3->clone());      
            return new conditional(op1_copy,op2_copy,op3_copy);     
        }                                           
    private:
        plural_ptr op1,op2,op3;
    };


    plural_ptr bin_factory(int value,plural_ptr left,plural_ptr right)
    {

        switch(value) {
        case '/':  return plural_ptr(new div(left,right));
        case '*':  return plural_ptr(new mul(left,right));
        case '%':  return plural_ptr(new mod(left,right));
        case '+':  return plural_ptr(new add(left,right));
        case '-':  return plural_ptr(new sub(left,right));
        case SHL:  return plural_ptr(new shl(left,right));
        case SHR:  return plural_ptr(new shr(left,right));
        case '>':  return plural_ptr(new  gt(left,right));
        case '<':  return plural_ptr(new  lt(left,right));
        case GTE:  return plural_ptr(new gte(left,right));
        case LTE:  return plural_ptr(new lte(left,right));
        case  EQ:  return plural_ptr(new  eq(left,right));
        case NEQ:  return plural_ptr(new neq(left,right));
        case '&':  return plural_ptr(new bin_and(left,right));
        case '^':  return plural_ptr(new bin_xor(left,right));
        case '|':  return plural_ptr(new bin_or (left,right));
        case AND:  return plural_ptr(new l_and(left,right));
        case  OR:  return plural_ptr(new l_or(left,right));
        default:
            return plural_ptr();
        }
    }

    static inline bool is_in(int v,int *p)
    {
        int len=*p;
        p++;
        while(len && *p!=v) { p++;len--; }
        return len!=0;
    }


    class tokenizer {
    public:
        tokenizer(char const *s) { text=s; pos=0; step(); };
        int get(int *val=NULL){
            int iv=int_value;
            int res=next_tocken;
            step();
            if(val && res==NUM){
                *val=iv;
            }
            return res;
        };
        int next(int *val=NULL) {
            if(val && next_tocken==NUM) {
                *val=int_value;
                return NUM;
            }
            return next_tocken;
        }
    private:
        char const *text;
        int pos;
        int next_tocken;
        int int_value;
        bool is_blank(char c)
        {
            return c==' ' || c=='\r' || c=='\n' || c=='\t';
        }
        bool isdigit(char c) 
        {
            return '0'<=c && c<='9'; 
        }
        void step() 
        {
            while(text[pos] && is_blank(text[pos])) pos++;
            char const *ptr=text+pos;
            char *tmp_ptr;
            if(strncmp(ptr,"<<",2)==0) { pos+=2; next_tocken=SHL; }
            else if(strncmp(ptr,">>",2)==0) { pos+=2; next_tocken=SHR; }
            else if(strncmp(ptr,"&&",2)==0) { pos+=2; next_tocken=AND; }
            else if(strncmp(ptr,"||",2)==0) { pos+=2; next_tocken=OR; }
            else if(strncmp(ptr,"<=",2)==0) { pos+=2; next_tocken=LTE; }
            else if(strncmp(ptr,">=",2)==0) { pos+=2; next_tocken=GTE; }
            else if(strncmp(ptr,"==",2)==0) { pos+=2; next_tocken=EQ; }
            else if(strncmp(ptr,"!=",2)==0) { pos+=2; next_tocken=NEQ; }
            else if(*ptr=='n') { pos++; next_tocken=VARIABLE; }
            else if(isdigit(*ptr)) { int_value=strtol(text+pos,&tmp_ptr,0); pos=tmp_ptr-text; next_tocken=NUM; }
            else if(*ptr=='\0') { next_tocken=0; }
            else { next_tocken=*ptr; pos++; }
        }
    };


    #define BINARY_EXPR(expr,hexpr,list)                            \
        plural_ptr expr()                                           \
        {                                                           \
            plural_ptr op1,op2;                                     \
            if((op1=hexpr()).get()==0)                              \
                return plural_ptr();                                \
            while(is_in(t.next(),list)) {                           \
                int o=t.get();                                      \
                if((op2=hexpr()).get()==0)                          \
                    return plural_ptr();                            \
                op1=bin_factory(o,op1,op2);                         \
            }                                                       \
            return op1;                                             \
        }

    class parser {
    public:

        parser(tokenizer &tin) : t(tin) {};

        plural_ptr compile()
        {
            plural_ptr res=cond_expr();
            if(res.get() && t.next()!=END) {
                return plural_ptr();
            };
            return res;
        }

    private:

        plural_ptr value_expr()
        {
            plural_ptr op;
            if(t.next()=='(') {
                t.get();
                if((op=cond_expr()).get()==0)
                    return plural_ptr();
                if(t.get()!=')')
                    return plural_ptr();
                return op;
            }
            else if(t.next()==NUM) {
                int value;
                t.get(&value);
                return plural_ptr(new number(value));
            }
            else if(t.next()==VARIABLE) {
                t.get();
                return plural_ptr(new identity());
            }
            return plural_ptr();
        };

        plural_ptr un_expr()
        {
            plural_ptr op1;
            static int level_unary[]={3,'-','!','~'};
            if(is_in(t.next(),level_unary)) {
                int op=t.get();
                if((op1=un_expr()).get()==0) 
                    return plural_ptr();
                switch(op) {
                case '-': 
                    return plural_ptr(new minus(op1));
                case '!': 
                    return plural_ptr(new l_not(op1));
                case '~': 
                    return plural_ptr(new bin_not(op1));
                default:
                    return plural_ptr();
                }
            }
            else {
                return value_expr();
            }
        };

        BINARY_EXPR(l10,un_expr,level10);
        BINARY_EXPR(l9,l10,level9);
        BINARY_EXPR(l8,l9,level8);
        BINARY_EXPR(l7,l8,level7);
        BINARY_EXPR(l6,l7,level6);
        BINARY_EXPR(l5,l6,level5);
        BINARY_EXPR(l4,l5,level4);
        BINARY_EXPR(l3,l4,level3);
        BINARY_EXPR(l2,l3,level2);
        BINARY_EXPR(l1,l2,level1);

        plural_ptr cond_expr()
        {
            plural_ptr cond,case1,case2;
            if((cond=l1()).get()==0)
                return plural_ptr();
            if(t.next()=='?') {
                t.get();
                if((case1=cond_expr()).get()==0)
                    return plural_ptr();
                if(t.get()!=':')
                    return plural_ptr();
                if((case2=cond_expr()).get()==0)
                    return plural_ptr();
            }
            else {
                return cond;
            }
            return plural_ptr(new conditional(cond,case1,case2));
        }

        tokenizer &t;

    };

} // namespace anon

plural_ptr compile(char const *str)
{
    tokenizer t(str);
    parser p(t);
    return p.compile();
}


} // lambda
} // gnu_gettext
} // locale
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

