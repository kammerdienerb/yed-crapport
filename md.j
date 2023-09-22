set md:__kwdargs__
    quote
        foreach var (keys KWDARGS)
            elocal
                symbol  var
                KWDARGS var

fn (md:unique-column-values column)
    do
        local values object
        foreach row @table
            if (and (in row column))
                insert values (row column) nil
        keys values

fn (md:avg-metric KWDARGS)
    do
        # groups: list of fields by which to hierarchically organize results
        # metric: field to report the average value of
        # norm:   value of top-level group to normalize against and omit (optional)
        md:__kwdargs__


        local donorm (in KWDARGS "norm")
        local normgroup nil
        if (and donorm (len groups))
            local normgroup (groups 0)

        if (not (in KWDARGS "extraname"))
            local extraname nil

        localfn (collect-groups groups metric normgroup norm match extraname)
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
                                        foreach column (keys normmatch)
                                            local matches
                                                and matches
                                                    in row metric
                                                    in row column
                                                    ==
                                                        row       column
                                                        normmatch column
                                        if matches
                                            then
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

                        foreach row @table
                            do
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
                                    then
                                        insert out "__sum__"
                                            + (out "__sum__") (row metric)
                                        insert out "__count__"
                                            + (out "__count__") 1

                        insert out metric
                            / (out "__sum__") (out "__count__")

                        if (!= nil base)
                            then
                                insert out metric
                                    / (out metric) (base metric)
                        if (!= nil extraname)
                            then
                                insert out extraname (out metric)
                    else
                        local group (groups 0)
                        erase groups 0

                        foreach value (md:unique-column-values group)
                            do
                                if (or (!= group normgroup) (!= norm value))
                                    then
                                        if (not (in out value))
                                            update-object match (object (. group value))

                                        insert out value (collect-groups groups metric normgroup norm match extraname)
                out

        collect-groups groups metric normgroup (if donorm norm nil) object extraname
