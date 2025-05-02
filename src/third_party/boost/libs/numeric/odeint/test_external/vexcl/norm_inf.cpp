#define BOOST_TEST_MODULE odeint_vexcl_norm_inf

#include <boost/numeric/odeint/external/vexcl/vexcl_norm_inf.hpp>
#include <boost/test/unit_test.hpp>

template <class T>
double norm(const T &x) {
    return boost::numeric::odeint::vector_space_norm_inf<T>()(x);
}

BOOST_AUTO_TEST_CASE( norm_inf )
{
    vex::Context ctx(vex::Filter::Env);
    std::cout << ctx << std::endl;

    vex::vector<double> x(ctx, 1024);
    x = 41;

    vex::multivector<double, 2> y(ctx, 1024);
    y = 42;

    BOOST_CHECK_EQUAL( norm(x), 41 );
    BOOST_CHECK_EQUAL( norm(y), 42 );
}

