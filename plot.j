use-package "math"

set plot:colors
    list
        @color "&active.bg swap"
        @color "&active"
        @color "&code-fn-call"
        @color "&code-keyword"
        @color "&code-string"
        @color "&code-comment"
        @color "&code-escape"
        @color "&code-number"
        @color "&red"
        @color "&turquoise"
        @color "&orange"
        @color "&cyan"
        @color "&yellow"
        @color "&blue"
        @color "&lime"
        @color "&purple"
        @color "&pink"
        @color "&green"
        @color "&magenta"

fn (plot:set-colors colors)
    set plot:colors colors

set plot:__kwdargs__
    quote
        foreach var (keys KWDARGS)
            elocal (symbol var) (KWDARGS var)

fn (plot:bar-2d KWDARGS)
    # point-objects: Object with a set of objects to plot. Each element object should have a field "y" (and optionally, "error").
    # order:         List of keys of point-objects, which dicatates the order of plotted points. (optional)
    md:__kwdargs__


    if (not (in KWDARGS "order"))
        local order (keys point-objects)

    local plot
        object
            . "type"         "bar"
            . "groups"       list
            . "fg"           (plot:colors 0)
            . "bg"           (plot:colors 1)
            . "point-labels" 1

    local i    0
    local x    0.5
    local ymax -9999999999

    foreach key order
        local point (point-objects key)
        local y     (point "y")

        if (> y ymax)
            local ymax y

        local plot-point
            object (. "x" x) (. "y" y)

        if (in point "error")
            insert plot-point "error" (point "error")

        append (plot "groups")
            object
                . "label"  key
                . "size"   0.75
                . "color"  (plot:colors (+ i 2))
                . "points" (list plot-point)

        local i (+ i 1)
        local x (+ x 1)

    insert plot "xmax" (- x 0.5)

    local j    0
    local ypow ymax
    while (> ypow 10)
        local ypow (/ ypow 10)
        local j    (+ j 1)

    local ymaxbase (+ (math:floor ypow) 1)
    local ymax     (* ymaxbase (math:pow 10 j))

    insert plot "ymax"   ymax
    insert plot "ymarks" ymaxbase

    plot
