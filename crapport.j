println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

@display-columns "run_config" "runtime" "lulesh_FOM" "amg_FOM" "date"

eval-file "md.j"
eval-file "plot.j"

fn (date-is-after d1 d2) (> (d1 "epoch") (d2 "epoch"))

@filter
    date-is-after
        iso-date (@row "date")
        iso-date "2023-09-18 0:0:0"

foreach row @table
    insert row "name"
        if (== (row "run_config") "mdrun")
            (row "policy_pack_string")
            (row "run_config")

foreach metric (list "runtime" "lulesh_FOM" "amg_FOM")
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
                    . "ymax"         1.5
                    . "ymarks"       3
                    . "width"        100
