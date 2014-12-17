#include <string>
#include <iostream>

//  Minimal class path

class path
{
public:
  path( const char * )
  {
    std::cout << "path( const char * )\n";
  }
  path( const std::string & )
  {
    std::cout << "path( std::string & )\n";
  }

//  for maximum efficiency, either signature must work
# ifdef BY_VALUE
  operator const std::string() const
# else
  operator const std::string&() const
# endif
  {
    std::cout << "operator string\n";
    return m_path;
  }

#ifdef NAMED_CONVERSION
  std::string string() const
  {
    std::cout << "std::string string() const\n";
    return m_path;
  }
#endif

private:
  std::string m_path;
};

bool operator==( const path &, const path & )
{
  std::cout << "operator==( const path &, const path & )\n";
  return true;
}

//  These are the critical use cases. If any of these don't compile, usability
//  is unacceptably degraded.

void f( const path & )
{
  std::cout << "f( const path & )\n";
}

int main()
{
  f( "foo" );
  f( std::string( "foo" ) );
  f( path( "foo" ) );

  std::cout << '\n';

  std::string s1( path( "foo" ) );
  std::string s2 = path( "foo" );
  s2 = path( "foo" );

#ifdef NAMED_CONVERSION
  s2 = path( "foo" ).string();
#endif

  std::cout << '\n';

  // these must call bool path( const path &, const path & );
  path( "foo" ) == path( "foo" );
  path( "foo" ) == "foo";
  path( "foo" ) == std::string( "foo" );
  "foo" == path( "foo" );
  std::string( "foo" ) == path( "foo" );

  return 0;
}
