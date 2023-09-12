set md:__kwdargs__
    quote
        foreach var KWDARGS
            elocal
                symbol var
                field  KWDARGS var

fn (md:unique-column-values column)
    do
        local values object
        foreach row @table
            if (in row column)
                insert values (field row column) nil
        keys values

fn (md:avg-metric KWDARGS)
    do
        # groups: list of fields by which to hierarchically organize results
        # metric: field to report the average value of
        # norm:   value of top-level group to normalize against and omit (optional)
        md:__kwdargs__

        if (not (in KWDARGS "norm"))
            local norm nil

        local normgroup nil
        if (and (len groups) (!= nil norm))
            local normgroup (elem groups 0)

        localfn (collect-groups groups metric normgroup norm match)
            do
                local out object
                if (empty groups)
                    then
                        local base nil

                        if (!= nil normgroup)
                            then
                                local base
                                    object
                                        . "__sum__"   0
                                        . "__count__" 0

                                local normmatch match
                                insert normmatch normgroup norm

                                foreach row @table
                                    do
                                        local matches 1
                                        foreach column normmatch
                                            local matches
                                                and matches
                                                    in row metric
                                                    in row column
                                                    ==
                                                        field row       column
                                                        field normmatch column
                                        if matches
                                            then
                                                insert base "__sum__"
                                                    + (field base "__sum__") (field row metric)
                                                insert base "__count__"
                                                    + (field base "__count__") 1
                                insert base metric
                                    / (field base "__sum__") (field base "__count__")

                        local out
                            object
                                . "__sum__"   0
                                . "__count__" 0

                        foreach row @table
                            do
                                local matches 1
                                foreach column match
                                    local matches
                                        and matches
                                            in row metric
                                            in row column
                                            ==
                                                field row   column
                                                field match column
                                if matches
                                    then
                                        insert out "__sum__"
                                            + (field out "__sum__") (field row metric)
                                        insert out "__count__"
                                            + (field out "__count__") 1

                        insert out metric
                            / (field out "__sum__") (field out "__count__")

                        if (!= nil base)
                            then
                                insert out metric
                                    / (field out metric) (field base metric)
                    else
                        local group (elem groups 0)
                        erase groups 0

                        foreach value (md:unique-column-values group)
                            do
                                if (or (!= group normgroup) (!= norm value))
                                    then
                                        if (not (in out value))
                                            update-object match (object (. group value))

                                        insert out value (collect-groups groups metric normgroup norm match)
                out

        collect-groups groups metric normgroup norm object
