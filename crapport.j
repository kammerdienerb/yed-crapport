add-package-directory "~/projects/jule/packages"

map (` use-package) (list "date" "math" "stats" "string")
map (` eval-file)   (list "md.j" "plot.j")

println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

eval-file "filters.j"

@display-columns "benchmark" "run_config" "fair_weight_regex"

foreach &row @table
    if (in &row "qmc_time")
        insert &row "qmcpack_time" (&row "qmc_time")

    set sp         (string:splits (&row "benchmark") "_and_")
    ref &first     (sp 0)
    ref &second    (sp 1)
    set first-fom  nil
    set second-fom nil

    set key (fmt "%_FOM" &first)
    if (and (in &row key) (!= nil (&row key)))
        set first-fom (&row key)
    else
        set key (fmt "%_time" &first)
        if (and (in &row key) (!= nil (&row key)))
            set first-fom (/ 1 (&row key))

    set key (fmt "%_FOM" &second)
    if (and (in &row key) (!= nil (&row key)))
        set second-fom (&row key)
    else
        set key (fmt "%_time" &second)
        if (and (in &row key) (!= nil (&row key)))
            set second-fom (/ 1 (&row key))

    set &rc (&row "run_config")
    set config-name nil

    if (== &rc "sys_malloc_32")
        set config-name "Default"
    elif (== &rc "autonuma")
        set config-name "Autonuma"
    elif (== &rc "cache_mode")
        set config-name "Memory Mode"
    elif (== &rc "mdrun")
        if (== nil (&row "fair_weight_regex"))
            set config-name "Guided Default"
        else
            local x (string:split (string:replace (&row "fair_weight_regex") "'" "") " ")
            if (== 1.0 (parse-float (x 0)))
                set config-name "Guided Fair"
            elif (== (x 1) &first)
                set config-name "Guided First Nice"
            elif (== (x 1) &second)
                set config-name "Guided Second Nice"

    insert &row "config"  config-name
    insert &row "benches" (fmt "%/%" &first &second)
    insert &row "FOM1"    first-fom
    insert &row "FOM2"    second-fom

set results nil
foreach metric (list "FOM1" "FOM2")
    set collect
        md:avg-metric
            object
                . "metric"    metric
                . "groups"    (list "config" "benches")
                . "norm"      "Default"
    if (== nil results)
        set results collect
    else
        foreach config (keys results)
            foreach benches (keys (results config))
                insert ((results config) benches)
                    metric
                    ((collect config) benches) metric

foreach config (keys results)
    ref &config (results config)
    foreach benchset (keys &config)
        ref &benchset (&config benchset)
        println (fmt "% % % %" config benchset (&benchset "FOM1") (&benchset "FOM2"))

println results
if 0
    set plot-args
        object
            . "point-objects" results
            . "keys"          (list "FOM1" "FOM2")
            . "order"         (sorted (keys results))
            . "legendx"       0.9
            . "legendy"       1.4

    @plot
        update-object (plot:bar plot-args)
            object
                . "title"        "FOM of 'nice' configurations relative to alloc/lru/hybrid_hotset_purge"
                . "ymax"         1.5
                . "ymarks"       6
                . "width"        150
