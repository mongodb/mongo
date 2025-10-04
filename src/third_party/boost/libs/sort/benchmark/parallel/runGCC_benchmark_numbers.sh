clear
echo "=================================================================="
echo "==              B E N C H M A R K   N U M B E R S               =="
echo "==                                                              =="
echo "==                 G C C      C O M P I L E R                   =="
echo "=================================================================="
echo "."
echo "C O M P I L I N G . . . . . . . . . . ."
echo "."
g++ ./file_generator.cpp -std=c++11 -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include  -s  -o file_generator

g++ ./benchmark_numbers.cpp -std=c++11 -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include -pthread  -s  -lpthread  -o benchmark_numbers

echo "R U N N I N G . . . . . . . . . . ."
echo "( The time needed is around 10 minutes depending of your machine )"
./file_generator input.bin 125000000
echo "."
date
./benchmark_numbers
date
rm input.bin
rm file_generator
rm benchmark_numbers
echo "."

echo "."
echo "E N D"
echo "."
