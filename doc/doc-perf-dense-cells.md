# Dense-cell output performance

Three approaches were evaluated. **Core X11 fills for opaque backgrounds**
showed no repeatable additional gain and were set aside; Xft already uses
`PictOpSrc`. The two useful changes are:

- **Frame timestamp:** Record the current time after X11 drawing and frame
  submission. Drawing can block longer than a refresh interval, making the
  event loop's cached timestamp stale. The fresh timestamp gives PTY output
  time to drain before another redraw, coalescing more output into fewer
  intermediate frames.
- **Glyph bounds:** Cache ASCII ink bounds and check other glyphs' extents.
  Skip Xft clip setup and clearing when ink fits within its row, retaining
  clipping for overflowing glyphs.

## Measurements

Run `./cycle.sh example.toml -b vtebench_dense_cells -t nrxvt` in te-stsuite.
Each variant was measured three times with `xft:monospace:size=11`, one
warmup, ten-second measurement windows, cgroupv2 accounting and taskset on
CPU 0. Core fills were disabled. The terminal command added
`-override-redirect -geometry 174x48+0+0`, fixing the payload at 5,863,182
bytes per sample. Execution order varied between repetitions.

Write times include PTY backpressure; completed-frame presentation was not
measured. CPU percentage and time cover the entire run; CPU/sample divides
CPU time by sample count. Memory columns are the suite's average and peak.

 | Optimizations enabled          | Run  | Samples | Write avg (ms) | CPU (%) | CPU time (ms) | CPU/sample (ms) | Memory avg (MiB) | Memory peak (MiB) |
 | ---                            | ---: | ---:    | ---:           | ---:    | ---:          | ---:            | ---:             | ---:              |
 | None                           | 1    | 19      | 558.42         | 24.41   | 2589.50       | 136.29          | 5.49             | 5.99              |
 | None                           | 2    | 16      | 645.33         | 22.82   | 2356.43       | 147.28          | 5.50             | 6.08              |
 | None                           | 3    | 20      | 559.34         | 24.28   | 2716.04       | 135.80          | 6.27             | 6.71              |
 | Frame timestamp                | 1    | 51      | 196.41         | 30.75   | 3080.04       | 60.39           | 6.97             | 7.46              |
 | Frame timestamp                | 2    | 52      | 195.84         | 31.32   | 3189.61       | 61.34           | 5.32             | 5.78              |
 | Frame timestamp                | 3    | 51      | 199.74         | 31.17   | 3175.99       | 62.27           | 5.51             | 6.02              |
 | Glyph bounds                   | 1    | 14      | 739.76         | 20.46   | 2118.58       | 151.33          | 6.07             | 6.54              |
 | Glyph bounds                   | 2    | 15      | 699.77         | 20.91   | 2195.45       | 146.36          | 5.35             | 5.86              |
 | Glyph bounds                   | 3    | 22      | 483.56         | 22.70   | 2414.64       | 109.76          | 6.80             | 7.36              |
 | Frame timestamp + glyph bounds | 1    | 54      | 185.40         | 30.53   | 3056.50       | 56.60           | 7.06             | 7.51              |
 | Frame timestamp + glyph bounds | 2    | 53      | 189.02         | 30.53   | 3058.98       | 57.72           | 6.89             | 7.33              |
 | Frame timestamp + glyph bounds | 3    | 53      | 189.56         | 30.57   | 3071.00       | 57.94           | 5.33             | 5.72              |

Together, the changes reduced mean write time from 587.70 ms to 187.99 ms,
about **3.1 times the throughput**. Adding glyph bounds to the timestamp
correction reduced mean write time by **4.7%** and CPU per sample by **6.4%**.
Glyph bounds alone remained variable. No consistent memory reduction was
observed.
