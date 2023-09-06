set md:__kwdargs__
    quote
        foreach VAR KWDARGS
            elocal
                symbol VAR
                field  KWDARGS VAR

fn
    md:avg-metric KWDARGS
    do
        # GROUPS: list of fields by which to hierarchically organize results
        # METRIC: field to report the average value of
        md:__kwdargs__

        local OUT object

        # Build a matrix of all possible values for each grouping field.
        local FIELD_VALS object
        foreach GROUP GROUPS
            do
                insert FIELD_VALS GROUP object
                foreach ROW @TABLE
                    do
                        if
                            in ROW GROUP
                            insert
                                field FIELD_VALS GROUP
                                field ROW        GROUP
                                nil
                insert FIELD_VALS GROUP
                    keys
                        field FIELD_VALS GROUP

        println FIELD_VALS

        localfn
            build-matrix
            do
                local NEWMATRIX object
                NEWMATRIX

        OUT
