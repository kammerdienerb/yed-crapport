println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

eval-file "md.j"
eval-file "plot.j"

set metric "peak_rss"
set groups (list "date")
set norm   "2023-08-30 08:50:25.233017"

set results
    md:avg-metric
        object
            . "metric" metric
            . "groups" groups
            . "norm"   norm
foreach item results (insert item "y" (field item metric))

set neg object
foreach date (keys results)
    do
        local new (field results date)
        insert new "y" (* (field new "y") -1)
        insert neg (fmt "%-neg" date) new

foreach date (keys neg)
    insert results date (field neg date)

set plot-args
    object
        . "point-objects" results
        . "order"         (sorted (keys results))

@plot
    update-object (plot:bar-2d plot-args)
        object
            . "ymax"         1.3
            . "ymin"         -1.3
            . "width"        100
#             . "appearx"      0.56
#             . "appeary"      0.45
