println
    fmt "*** CRAPPORT (% rows, % columns) ***"
        len @TABLE
        len @COLUMNS

# eval-file "md.j"

# @here: I can reproduce the crash with one dummy object in @TABLE.
#        Take to a machine with valgrind.

set @TABLE
    list
        object
            . "run_config" "autonuma"
            . "benchmark"  "warpx"
            . "runtime"    1000

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

        localfn
            collect-groups GROUPS MATCH
            do
                if
                    len GROUPS
                    do
                        local OUT object

                        local GROUP
                            pop GROUPS

                        local VALUES object
                        foreach ROW @TABLE
                            if
                                in ROW GROUP
                                insert VALUES
                                    field ROW GROUP
                                    nil

                        foreach VALUE VALUES
                            do
                                local THISGROUP object

                                local MATCHER
                                    MATCH
                                insert MATCHER GROUP VALUE

                                foreach GROUPED
                                    collect-groups GROUPS MATCHER
                                    do
                                        insert THISGROUP VALUE GROUPED

                    foreach ROW @TABLE
                        do
                            local MATCHES 1
                            foreach COLUMN ENTRY
                                local MATCHES
                                    and MATCHES
                                        in ROW METRIC
                                        in ROW COLUMN
                                        ==
                                            field ROW   COLUMN
                                            field ENTRY COLUMN

        local FOO
            collect-groups GROUPS object

        OUT

#         # Build a matrix of all possible values for each grouping field.
#         localfn
#             build-matrix GROUPS
#             do
#                 if
#                     len GROUPS
#                     do
#                         local GROUP
#                             pop GROUPS
#                         local MATRIX
#                             build-matrix GROUPS

#                         local VALUES object
#                         foreach ROW @TABLE
#                             if
#                                 in ROW GROUP
#                                 insert VALUES
#                                     field ROW GROUP
#                                     nil

#                         local NEWMATRIX list
#                         if
#                             ==
#                                 len MATRIX
#                                 0
#                             do
#                                 foreach VALUE VALUES
#                                         do
#                                             local NEWENTRY
#                                                 object
#                                                     . GROUP VALUE
#                                             append NEWMATRIX NEWENTRY
#                             do
#                                 foreach VALUE VALUES
#                                     foreach ENTRY MATRIX
#                                         do
#                                             local NEWENTRY ENTRY
#                                             insert NEWENTRY GROUP VALUE
#                                             append NEWMATRIX NEWENTRY
#                         NEWMATRIX
#                     list

#         local MATRIX
#             build-matrix GROUPS

#         foreach ENTRY MATRIX
#             do
#                 local RESULT
#                     object
#                         . "__SUM__"   0
#                         . "__COUNT__" 0

#                 foreach ROW @TABLE
#                     do
#                         local MATCHES 1
#                         foreach COLUMN ENTRY
#                             local MATCHES
#                                 and MATCHES
#                                     in ROW METRIC
#                                     in ROW COLUMN
#                                     ==
#                                         field ROW   COLUMN
#                                         field ENTRY COLUMN
#                         if MATCHES
#                             do
#                                 insert RESULT "__COUNT__"
#                                     +
#                                         1
#                                         field RESULT "__COUNT__"
#                                 insert RESULT "__SUM__"
#                                     +
#                                         field ROW    METRIC
#                                         field RESULT "__SUM__"
#                 update-object ENTRY
#                     object
#                         .   METRIC
#                             /
#                                 field RESULT "__SUM__"
#                                 field RESULT "__COUNT__"
#                         .   "__COUNT__"
#                             field RESULT "__COUNT__"

#         local OUT object
#         foreach GROUP GROUPS
#             do
#                 local VALUES object
#                 foreach ENTRY MATRIX
#                     insert VALUES
#                         field ENTRY GROUP
#                         nil
#

#         MATRIX

# @filter
#     and
#         !=
#             field @ROW "benchmark"
#             "snap"
#         ==
#             field @ROW "input_size"
#             "large"
#         ==
#             field @ROW "iteration"
#             21
#         !=
#             field @ROW "name"
#             "it-large-md-avg-bad"
#         !=
#             field @ROW "name"
#             "it-large-md-ind"

println
    md:avg-metric
        object
            . "GROUPS"
                        list "run_config" # "benchmark"
            . "METRIC"
                        "runtime"


set BYCONFIG object
set BENCHES  list
foreach ROW @TABLE
    do
        set CONFIG
            field ROW "run_config"
        set BENCH
            field ROW "benchmark"
        if
            not
                in BYCONFIG CONFIG
            insert BYCONFIG CONFIG object
        if
            not
                in BENCHES BENCH
            append BENCHES BENCH

fn
    bench-offset BENCH
    do
        index BENCHES BENCH

fn
    config-offset CONFIG
    do
        local CONFIG_OFFSETS
            object
                . "sys_malloc_32" 0
                . "cache_mode"    1
                . "autonuma"      2
                . "mdrun"         3
        field CONFIG_OFFSETS CONFIG

fn
    get-x CONFIG BENCH
    do
        +
            *
                +   0.5
                    -
                        len BYCONFIG
                        1
                bench-offset BENCH
            config-offset CONFIG

foreach ROW @TABLE
    do
        set BENCH
            field ROW "benchmark"
        set CONFIG
            field ROW "run_config"
        if
            == CONFIG "sys_malloc_32"
            insert
                field BYCONFIG CONFIG
                BENCH
                object
                    . "x"
                            get-x CONFIG BENCH
                    . "y"
                            field ROW "runtime"

foreach ROW @TABLE
    do
        set BENCH
            field ROW "benchmark"
        set CONFIG
            field ROW "run_config"
        if
            != CONFIG "sys_malloc_32"
            insert
                field BYCONFIG CONFIG
                BENCH
                object
                    . "x"
                            get-x CONFIG BENCH
                    . "y"
                            /
                                field ROW "runtime"
                                field
                                    field
                                        field BYCONFIG "sys_malloc_32"
                                        BENCH
                                    "y"

### Plot results ###
println "Plotting..."

set COLORS
    list
        @color "&code-fn-call"
        @color "&code-keyword"
        @color "&code-string"
        @color "&code-comment"
        @color "&code-escape"
        @color "&code-number"

set XMAX
    +   3
        *
            -
                len BYCONFIG
                1
            len BENCHES

set YMAX 1.0

set GROUPS list
foreach CONFIG
    keys BYCONFIG
    do
        if
            != CONFIG "sys_malloc_32"
            append GROUPS
                object
                    . "points"
                                values
                                    field BYCONFIG CONFIG
                    . "color"
                                elem    COLORS
                                        config-offset CONFIG
                    . "size"    1
                    . "label"   CONFIG
                    . "labelx"  -1
                    . "labely"
                                *
                                    *   -0.035
                                        YMAX

                                    +   1
                                        config-offset CONFIG

foreach BENCH BENCHES
    append GROUPS
        object
            . "label"   BENCH
            . "labelx"
                        get-x "autonuma" BENCH
            . "labely"
                        *   -0.04
                            YMAX

set PLOT
    object
        . "type"          "bar"
        . "title"         "Test Graph"
        . "width"         100
        . "height"        80
        . "fg"
                          @color "&active"
        . "bg"
                          @color "&active.bg swap"
        . "xmax"          XMAX
        . "ymax"          YMAX
        . "xmarks"        0
        . "ymarks"        4
        . "point-labels"  1
        . "invert-labels" 0
        . "groups"        GROUPS
        . "appearx"       0.691
        . "appeary"       0.54

println "Done."
