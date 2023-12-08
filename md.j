use-package "math"
use-package "stats"

set md:__kwdargs__
    quote
        foreach var (keys KWDARGS)
            eref (symbol var) (KWDARGS var)
#             elocal (symbol var) (KWDARGS var)

fn (md:unique-column-values column)
    local values object
    foreach row @table
        if (in row column)
            insert values (row column) nil
    keys values

fn (md:avg-metric KWDARGS)
    # groups: list of fields by which to hierarchically organize results
    # metric: field to report the average value of
    # norm:   value of top-level group to normalize against and omit (optional)
    md:__kwdargs__


    local donorm (in KWDARGS "norm")
    local normgroup nil
    if (and donorm (len groups))
        local normgroup (groups 0)

    localfn (collect-groups groups metric normgroup norm match)
        local out object

        if (empty groups)
            local base nil

            if (!= nil normgroup)
                local base
                    object
                        . "__sum__"   0
                        . "__count__" 0

                local normmatch match
                insert normmatch normgroup norm

                foreach row @table
                    local matches 1
                    foreach column (keys normmatch)
                        local matches
                            and matches
                                in row metric
                                in row column
                                ==
                                    row       column
                                    normmatch column
                    if matches
                        insert base "__sum__"
                            + (base "__sum__") (row metric)
                        insert base "__count__"
                            + (base "__count__") 1
                insert base metric
                    / (base "__sum__") (base "__count__")

            local out
                object
                    . "__sum__"   0
                    . "__count__" 0

            local values list

            foreach row @table
                local matches 1
                foreach column (keys match)
                    local matches
                        and matches
                            in row metric
                            in row column
                            ==
                                row   column
                                match column
                if matches
                    insert out "__sum__"
                        + (out "__sum__") (row metric)
                    insert out "__count__"
                        + (out "__count__") 1
                    append values (row metric)

            insert out metric
                / (out "__sum__") (out "__count__")

            if (!= nil base)
                insert out metric
                    / (out metric) (base metric)

                local normvalues
                    map (lambda (v) (/ v (base metric))) values

                insert out "error"
                    /   (stats:std normvalues)
                        (math:sqrt (len normvalues))
        else
            local group (groups 0)
            erase groups 0

            foreach value (md:unique-column-values group)
                if (or (!= group normgroup) (!= norm value))
                    if (not (in out value))
                        update-object match (object (. group value))

                    insert out value (collect-groups groups metric normgroup norm match)
        out

    collect-groups groups metric normgroup (select donorm norm nil) object
