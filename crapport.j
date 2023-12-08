add-package-directory "~/projects/jule/packages"

map (` use-package) (list "date" "math" "stats" "string")
map (` eval-file)   (list "md.j" "plot.j")

println (fmt "*** CRAPPORT (% rows, % columns) ***" (len @table) (len @columns))

eval-file "filters.j"

@display-columns "benchmark" "fair_weight_regex" "runtime" "lulesh_FOM" "amg_FOM"

foreach row @table
    if (== nil (row "fair_weight_regex"))
        insert row "name" "default-guided"
    else
        local sp (string:split (string:replace (row "fair_weight_regex") "'" "") " ")
        insert row "name" (fmt "% %" (sp 1) (sp 0))

set results nil
foreach metric (list "lulesh_FOM" "amg_FOM")
    set collect
        md:avg-metric
            object
                . "metric"    metric
                . "groups"    (list "name")
                . "norm"      "default-guided"
    if (== nil results)
        set results collect
    else
        foreach key (keys results)
            insert (results key)
                metric
                (collect key) metric

if 0
    set plot-args
        object
            . "point-objects" results
            . "keys"          (list "lulesh_FOM" "amg_FOM")
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
