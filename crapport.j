add-package-directory "~/projects/jule/packages"

map (` use-package) (list "date" "math" "stats" "string")
map (` eval-file)   (list "md.j" "plot.j")

println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

### Priority
# @filter
#     or  (== (@row "name") "priority_fixed")
#         and (== (@row "name") "priority")
#             (!= (string:index (@row "benchmark") "amg_low") 0)
#             (== nil (@row "mdadv"))
#             (!= "hybrid_hotset_purge_lockout" (@row "policy_pack_string"))
@filter (== "priority_final" (@row "name"))
@display-columns "benchmark" "numactl_low_priority" "run_config" "mdadv" "qmcpack_time" "amg_FOM" "lulesh_FOM" "qmcpack_high_time" "warpx_time"

foreach &row @table
    if (== "qmcpack_low_qmcpack_high" (&row "benchmark"))
        println (&row "crapport_dirname")

foreach &row @table
    if (!= nil (&row "numactl_low_priority"))
        insert &row "config" "numactl"
    elif (!= nil (&row "mdadv"))
        insert &row "config" "MD-priority"
    else
        insert &row "config" "MD-default"

    foreach &FOM (list "amg_FOM" "lulesh_FOM" "qmcpack_high_time" "warpx_time")
        if (!= nil (&row &FOM))
            if (or (== "qmcpack_high_time" &FOM) (== "warpx_time" &FOM))
                insert &row "high-FOM" (/ 1 (&row &FOM))
            else
                insert &row "high-FOM" (&row &FOM)

    insert &row "low-FOM" (/ 1 (&row "qmcpack_time"))

    set high-names
        object
            . "qmcpack_low_amg_high"     "amg"
            . "qmcpack_low_lulesh_high"  "lulesh"
            . "qmcpack_low_qmcpack_high" "qmcpack"
            . "qmcpack_low_warpx_high"   "warpx"

    insert &row "high-bench" (high-names (&row "benchmark"))


set results nil
foreach metric (list "low-FOM" "high-FOM")
    set collect
        md:avg-metric
            object
                . "metric"    metric
                . "groups"    (list "config" "high-bench")
                . "norm"      "numactl"
    if (== nil results)
        set results collect
    else
        foreach config (keys results)
            foreach benches (keys (results config))
                insert ((results config) benches)
                    metric
                    ((collect config) benches) metric


set data-points object

foreach config (keys results)
    set &benches (results config)
    foreach bench-name (keys &benches)
        set &bench (&benches bench-name)
        if (not (in data-points bench-name))
            insert data-points bench-name object
        update-object (data-points bench-name)
            object
                . (fmt "%-low-FOM" config)  (&bench "low-FOM")
                . (fmt "%-high-FOM" config) (&bench "high-FOM")

# set plot-args
#     object
#         . "point-objects" data-points
#         . "ykeys"         (list "MD-default-high-FOM" "MD-priority-high-FOM")
#         . "order"         (list "amg" "lulesh" "qmcpack" "warpx")
#         . "legendx"       0.1
#         . "legendy"       1.5
#         . "color-invert?" 1

# @plot
#     update-object (plot:bar plot-args)
#         object
#             . "title"        "High Priority Process FOM (relative to numactl)"
#             . "ymax"         1.5
#             . "ymarks"       3
#             . "width"        128
#             . "height"       64

# set plot-args
#     object
#         . "point-objects" data-points
#         . "ykeys"         (list "MD-default-low-FOM" "MD-priority-low-FOM")
#         . "order"         (list "amg" "lulesh" "qmcpack" "warpx")
#         . "legendx"       0.1
#         . "legendy"       1.5
#         . "color-invert?" 1

# @plot
#     update-object (plot:bar plot-args)
#         object
#             . "title"        "Low Priority Process FOM (relative to numactl)"
#             . "ymax"         1.5
#             . "ymarks"       3
#             . "width"        128
#             . "height"       64



### Site Advice
# @filter
#     and (== "large" (@row "input_size"))
#         (== "hybrid_hotset_purge" (@row "policy_pack_string"))
#         or  (== "site_advice" (@row "name"))
#             (== "backtrace_advice" (@row "name"))

# @display-columns "crapport_dirname" "name" "exit_status" "mdadv" "runtime"
