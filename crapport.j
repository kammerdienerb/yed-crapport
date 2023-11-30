add-package-directory "~/projects/jule/packages"

map (` use-package) (list "date" "math" "stats")
map (` eval-file)   (list "md.j" "plot.j")

println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

# @display-columns "date" "benchmark" "run_config" "policy_rank_string" "apb_nice_regex" "policy_pack_string" "runtime" "lulesh_FOM" "amg_FOM"
@display-columns "crapport_dirname" "run_config" "policy_rank_string" "apb_nice_regex" "policy_pack_string" "pack_nice_regex" "pack_purge_percent" "runtime" "qmc_time" "amg_FOM"


fn (date-before month day)
    or (< ((date:parse-iso (@row "date")) "month") month)
       (< ((date:parse-iso (@row "date")) "day")   day)

# @filter
#     and
#         == (@row "benchmark") "qmcpack_and_lulesh"
#         not (date-before 10 5)

@filter
    and
        != nil (@row "amg_FOM")
        == (@row "benchmark") "qmcpack_and_amg"
        or (!= (@row "run_config") "mdrun") (== (@row "policy_rank_string") "lru")
# @filter
#     or
#         (< ((date:parse-iso (@row "date")) "month") 10)
#         (< ((date:parse-iso (@row "date")) "day")   5)





if 0
    foreach row @table
        insert row "name"
            select (== (row "run_config") "mdrun")
                fmt "% (%\%)" (row "policy_pack_string") (row "pack_purge_percent")
                (row "run_config")

    foreach metric (list "runtime" "qmc_time" "amg_FOM")
        do
            set results
                md:avg-metric
                    object
                        . "metric"    metric
                        . "groups"    (list "name")
                        . "norm"      "sys_malloc_32"
                        . "extraname" "y"

            set plot-args
                object
                    . "point-objects" results
                    . "order"         (sorted (keys results))

            @plot
                update-object (plot:bar-2d plot-args)
                    object
                        . "title"        metric
                        . "ymax"         1.75
                        . "ymarks"       7
                        . "width"        150
