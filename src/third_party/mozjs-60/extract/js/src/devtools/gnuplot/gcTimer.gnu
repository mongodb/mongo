# gnuplot script to visualize GCMETER results.
# usage: "gnuplot gcTimer.gnu >outputfile.png"

set terminal png
# set Title
set title "Title goes here!"
set datafile missing "-"
set noxtics
#set ytics nomirror
set ylabel "msec"
set key below
set style data linespoints

#set data file
plot 'gcTimer.dat' using 2 title columnheader(2), \
'' u 3 title columnheader(3) with points, \
'' u 4 title columnheader(4), \
'' u 5 title columnheader(5), \
'' u 6 title columnheader(6) with points, \
'' u 7 title columnheader(7) with points, \
'' u 8 title columnheader(8) with points, \
'' u 9 title columnheader(9) with points, \
'' u 10 title columnheader(10) with points, \
'' u 11 title columnheader(11) with points
