println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))
# @display-columns "benchmark" "run_config" "runtime"



eval-file "md.j"



@filter
    and
        ==  21      (field @row "iteration")
        ==  "large" (field @row "input_size")
        !=  "snap"  (field @row "benchmark")



set rt
    md:avg-metric
        object
            . "metric" "runtime"
            . "groups" (list "run_config" "benchmark")
#             . "norm"   "sys_malloc_32"

println rt



### Plot results ###
println "Plotting..."

set groups object

set colors
    list
        @color "&code-fn-call"
        @color "&code-keyword"
        @color "&code-string"
        @color "&code-comment"
        @color "&code-escape"
        @color "&code-number"

set plot
    object
        . "type"          "bar"
        . "title"         "Test Graph"
        . "width"         100
        . "height"        80
        . "fg"
                          @color "&active"
        . "bg"
                          @color "&active.bg swap"
#         . "xmax"          XMAX
#         . "ymax"          YMAX
        . "xmarks"        0
        . "ymarks"        4
        . "point-labels"  1
        . "invert-labels" 0
        . "groups"        groups
#         . "appearx"       0.691
#         . "appeary"       0.54

# @plot plot

println "Done."
