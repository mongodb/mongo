clear
echo "=================================================================="
echo "==              B E N C H M A R K   N U M B E R S               =="
echo "==                                                              =="
echo "==               C L A N G      C O M P I L E R                 =="
echo "=================================================================="
echo "."
echo "C O M P I L I N G . . . . . . . . . . ."
echo "."
clang++ ./file_generator.cpp -std=c++11  -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include  -s  -o file_generator

clang++ ./benchmark_numbers.cpp -std=c++11 -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include -s -o benchmark_numbers

echo "R U N N I N G . . . . . . . . . . ."
echo "( The time needed is around 10 minutes depending of your machine )"
./file_generator input.bin 125000000
echo "."
date
./benchmark_numbers
echo "."
date
rm input.bin
rm file_generator
rm benchmark_numbers
echo "."
echo "E N D"
echo "."
