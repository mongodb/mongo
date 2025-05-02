clear
echo "=================================================================="
echo "==              B E N C H M A R K   O B J E C T S               =="
echo "==                                                              =="
echo "==               C L A N G    C O M P I L E R                   =="
echo "=================================================================="
echo "."
echo "C O M P I L I N G . . . . . . . . . . ."

clang++ ./file_generator.cpp -std=c++11 -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include  -s  -o file_generator

clang++ ./benchmark_objects.cpp -std=c++11 -march=native -w -fno-exceptions -fno-operator-names -O3 -I../../include -s  -o benchmark_objects
echo "."
echo "R U N N I N G . . . . . . . . . . ."
echo "( The time needed is around 45 minutes depending of your machine )"
./file_generator input.bin 125000000
echo "."
date
./benchmark_objects
date
echo "."
rm input.bin
rm file_generator
rm benchmark_objects

echo "."
echo "E N D"
echo "."
