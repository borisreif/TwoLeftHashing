set datafile separator comma
set terminal pngcairo size 1200,800 enhanced font 'Arial,12'
set output 'benchmarks/results.png'

set title 'TwoLeftHashMap vs std::unordered_map'
set xlabel 'Number of inserted elements'
set ylabel 'Nanoseconds per operation'
set logscale x 2
set grid
set key outside

plot \
    'benchmarks/results.csv' using 1:2 with linespoints title 'TwoLeft insert', \
    'benchmarks/results.csv' using 1:3 with linespoints title 'unordered_map insert', \
    'benchmarks/results.csv' using 1:4 with linespoints title 'TwoLeft find hit', \
    'benchmarks/results.csv' using 1:5 with linespoints title 'unordered_map find hit'
