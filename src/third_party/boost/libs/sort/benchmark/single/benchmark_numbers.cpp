//----------------------------------------------------------------------------
/// @file benchmark_numbers.cpp
/// @brief Benchmark of several sort methods with integer objects
///
/// @author Copyright (c) 2017 Francisco José Tapia (fjtapia@gmail.com )\n
///         Distributed under the Boost Software License, Version 1.0.\n
///         ( See accompanying file LICENSE_1_0.txt or copy at
///           http://www.boost.org/LICENSE_1_0.txt )
///
///         This program use for comparison purposes, the Threading Building
///         Blocks which license is the GNU General Public License, version 2
///         as  published  by  the  Free Software Foundation.
///
/// @version 0.1
///
/// @remarks
//-----------------------------------------------------------------------------
#include <algorithm>

#include <iostream>
#include <iomanip>
#include <random>
#include <stdlib.h>
#include <vector>

#include <boost/sort/common/time_measure.hpp>
#include <boost/sort/common/file_vector.hpp>
#include <boost/sort/common/int_array.hpp>

#include <boost/sort/sort.hpp>

#define NELEM 100000000

using namespace std;
namespace bsort = boost::sort;
namespace bsc = boost::sort::common;

using bsc::time_point;
using bsc::now;
using bsc::subtract_time;
using bsc::fill_vector_uint64;
using bsc::write_file_uint64;

using bsort::spinsort;
using bsort::flat_stable_sort;
using bsort::spreadsort::spreadsort;
using bsort::pdqsort;

void Generator_random (void);
void Generator_sorted (void);
void Generator_sorted_end (uint64_t n_last);
void Generator_sorted_middle (uint64_t n_middle);
void Generator_reverse_sorted (void);
void Generator_reverse_sorted_end (uint64_t n_last);
void Generator_reverse_sorted_middle (uint64_t n_middle);

void Test (const std::vector <uint64_t> &B);

int main (int argc, char *argv[])
{
    cout << "\n\n";
    cout << "************************************************************\n";
    cout << "**                                                        **\n";
    cout << "**               B O O S T      S O R T                   **\n";
    cout << "**              S I N G L E    T H R E A D                **\n";
    cout << "**          I N T E G E R    B E N C H M A R K            **\n";
    cout << "**                                                        **\n";
    cout << "**        SORT OF 100 000 000 NUMBERS OF 64 BITS          **\n";
    cout << "**                                                        **\n";
    cout << "************************************************************\n";
    cout << std::endl;

    cout<<"[ 1 ] std::sort   [ 2 ] pdqsort          [ 3 ] std::stable_sort \n";
    cout<<"[ 4 ] spinsort    [ 5 ] flat_stable_sort [ 6 ] spreadsort\n\n";
    cout<<"                    |      |      |      |      |      |      |\n";
    cout<<"                    | [ 1 ]| [ 2 ]| [ 3 ]| [ 4 ]| [ 5 ]| [ 6 ]|\n";
    cout<<"--------------------+------+------+------+------+------+------+\n";
    std::string empty_line =
           "                    |      |      |      |      |      |      |\n";
    cout<<"random              |";
    Generator_random ();
    cout<<empty_line;
    cout<<"sorted              |";
    Generator_sorted ();

    cout<<"sorted + 0.1% end   |";
    Generator_sorted_end (NELEM / 1000);

    cout<<"sorted +   1% end   |";
    Generator_sorted_end (NELEM / 100);

    cout<<"sorted +  10% end   |";
    Generator_sorted_end (NELEM / 10);

    cout<<empty_line;
    cout<<"sorted + 0.1% mid   |";
    Generator_sorted_middle (NELEM / 1000);

    cout<<"sorted +   1% mid   |";
    Generator_sorted_middle (NELEM / 100);

    cout<<"sorted +  10% mid   |";
    Generator_sorted_middle (NELEM / 10);

    cout<<empty_line;
    cout<<"reverse sorted      |";
    Generator_reverse_sorted ();

    cout<<"rv sorted + 0.1% end|";
    Generator_reverse_sorted_end (NELEM / 1000);

    cout<<"rv sorted +   1% end|";
    Generator_reverse_sorted_end (NELEM / 100);

    cout<<"rv sorted +  10% end|";
    Generator_reverse_sorted_end (NELEM / 10);

    cout<<empty_line;
    cout<<"rv sorted + 0.1% mid|";
    Generator_reverse_sorted_middle (NELEM / 1000);

    cout<<"rv sorted +   1% mid|";
    Generator_reverse_sorted_middle (NELEM / 100);

    cout<<"rv sorted +  10% mid|";
    Generator_reverse_sorted_middle (NELEM / 10);
    cout<<"--------------------+------+------+------+------+------+------+\n";
    cout<<endl<<endl ;
    return 0;
}
void Generator_random (void)
{
    vector <uint64_t> A;
    A.reserve (NELEM);
    A.clear ();
    if (fill_vector_uint64 ("input.bin", A, NELEM) != 0)
    {
        std::cout << "Error in the input file\n";
        std::exit (EXIT_FAILURE);
    };
    Test (A);
};
void Generator_sorted (void)
{
    vector<uint64_t> A;
    A.reserve (NELEM);
    A.clear ();
    for (uint64_t i = 0; i < NELEM; ++i)
        A.push_back (i);
    Test (A);
};
void Generator_sorted_end (uint64_t n_last)
{
    vector<uint64_t> A;
    A.reserve (NELEM);
    A.clear ();
    if (fill_vector_uint64 ("input.bin", A, NELEM + n_last) != 0)
    {
        std::cout << "Error in the input file\n";
        std::exit (EXIT_FAILURE);
    };
    std::sort (A.begin (), A.begin () + NELEM);
    Test (A);
};
void Generator_sorted_middle (uint64_t n_middle)
{
    assert (n_middle > 1 && NELEM >= (n_middle -1));
    vector <uint64_t> A, aux;
    A.reserve (NELEM + n_middle);
    aux.reserve (n_middle);

    if (fill_vector_uint64 ("input.bin", A, NELEM + n_middle) != 0)
    {
        std::cout << "Error in the input file\n";
        std::exit (EXIT_FAILURE);
    };
    for (uint64_t i = 0; i < n_middle; ++i)   aux.push_back (A [i]);

    std::sort (A.begin () + n_middle, A.end ());
    //------------------------------------------------------------------------
    // To insert n_middle elements, must have (n_middle - 1) intervals between
    // them. The size of the interval is step
    // The elements after the last element of aux don't need to be moved
    //-------------------------------------------------------------------------
    uint64_t step = NELEM / (n_middle - 1);
    A [0] = aux [0];
    uint64_t pos_read = n_middle, pos_write = 1;

    for (uint64_t i = 1; i < n_middle; ++i)
    {
        for (uint64_t k = 0 ; k < step; ++k)
            A [pos_write ++] = A [pos_read ++];
        A [pos_write ++] = aux [i];    
    };
    aux.clear ();
    aux.reserve (0);
    Test (A);
};
void Generator_reverse_sorted (void)
{
    vector<uint64_t> A;
    A.reserve (NELEM);
    A.clear ();
    for (uint64_t i = NELEM; i > 0; --i)
        A.push_back (i);
    Test (A);
};
void Generator_reverse_sorted_end (uint64_t n_last)
{
    vector <uint64_t> A;
    A.reserve (NELEM);
    A.clear ();
    if (fill_vector_uint64 ("input.bin", A, NELEM + n_last) != 0)
    {
        std::cout << "Error in the input file\n";
        std::exit (EXIT_FAILURE);
    };
    std::sort (A.begin (), A.begin () + NELEM);
    for (uint64_t i = 0; i < (NELEM >> 1); ++i)
        std::swap (A [i], A [NELEM - 1 - i]);

    Test (A);
}
void Generator_reverse_sorted_middle (uint64_t n_middle)
{
    assert (n_middle > 1 && NELEM >= (n_middle -1));
    vector <uint64_t> A, aux;
    A.reserve (NELEM + n_middle);
    aux.reserve (n_middle);

    if (fill_vector_uint64 ("input.bin", A, NELEM + n_middle) != 0)
    {
        std::cout << "Error in the input file\n";
        std::exit (EXIT_FAILURE);
    };
    for (uint64_t i = 0; i < n_middle; ++i)   aux.push_back (A [i]);

    std::sort (A.begin () + n_middle, A.end ());
    uint64_t pos1 = n_middle, pos2 = A.size () - 1;
    for (uint64_t i = 0; i < (NELEM >> 1); ++i)
        std::swap (A [pos1 ++], A [pos2 --]);
    //------------------------------------------------------------------------
    // To insert n_middle elements, must have (n_middle - 1) intervals between
    // them. The size of the interval is step
    // The elements after the last element of aux don't need to be moved
    //-------------------------------------------------------------------------
    uint64_t step = NELEM / (n_middle - 1);
    A [0] = aux [0];
    uint64_t pos_read = n_middle, pos_write = 1;

    for (uint64_t i = 1; i < n_middle; ++i)
    {
        for (uint64_t k = 0 ; k < step; ++k)
            A [pos_write ++] = A [pos_read ++];
        A [pos_write ++] = aux [i];    
    };
    aux.clear ();
    aux.reserve (0);
    Test (A);
};
void Test (const std::vector <uint64_t> &B)
{   
    //---------------------------- begin --------------------------------
    std::less <uint64_t> comp ;
    double duration;
    time_point start, finish;
    std::vector <uint64_t> A (B);
    std::vector <double> V;

    //--------------------------------------------------------------------
    A = B;
    start = now ();
    std::sort (A.begin (), A.end (), comp);
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    A = B;
    start = now ();
    pdqsort (A.begin (), A.end (), comp);
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    A = B;
    start = now ();
    std::stable_sort (A.begin (), A.end (), comp);
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    A = B;
    start = now ();
    spinsort(A.begin (), A.end (), comp);
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    A = B;
    start = now ();
    flat_stable_sort (A.begin (), A.end (), comp);
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    A = B;
    start = now ();
    spreadsort (A.begin (), A.end ());
    finish = now ();
    duration = subtract_time (finish, start);
    V.push_back (duration);

    //-----------------------------------------------------------------------
    // printing the vector
    //-----------------------------------------------------------------------
    std::cout<<std::setprecision (2) <<std::fixed;
    for ( uint32_t i =0 ; i < V.size () ; ++i)
    {   
        std::cout<<std::right<<std::setw (5)<<V [i]<<" |";
    };
    std::cout<<std::endl;
};
