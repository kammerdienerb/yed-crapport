use-package "math"

set plot:colors
    list
        @color "&active"
        @color "&active.bg swap"
        @color "&code-fn-call"
        @color "&code-string"
        @color "&code-keyword"
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
            eref (symbol var) (KWDARGS var)

fn (plot:bar KWDARGS)
    # point-objects: Object with a set of objects to plot. Each element object should have a field "y" (and optionally, "error").
    # keys:          List of keys in each point object to plot as y values.
    # order:         List of keys of point-objects, which dicatates the order of plotted points. (optional)
    # legendx:       The legend's x position on a 0.0-1.0 scale. (optional)
    # legendy:       The legend's y position in data scale (i.e. matching your plot points). (optional)
    # color-invert?: Boolean that flips the foreground and background colors. (optional)
    md:__kwdargs__

    local num     (len keys)
    local single? (== 1 num)

    if (not (in KWDARGS "order"))
        local order (keys point-objects)

    if (not (in KWDARGS "color-invert?"))
        local color-invert? 0

    ref &fg-color (plot:colors (select color-invert? 1 0))
    ref &bg-color (plot:colors (select color-invert? 0 1))

    local plot
        object
            . "type"         "bar"
            . "groups"       list
            . "fg"           &fg-color
            . "bg"           &bg-color
            . "point-labels" 1

    local x    0.5
    local ymax -9999999999

    local i 0
    foreach key order
        ref &point (point-objects key)

        if (not single?)
            append (plot "groups")
                object
                    . "label"  key
                    . "labelx" (+ x 0.25)
                    . "color"  &fg-color

        local j 0
        foreach y-key keys
            ref &y (&point y-key)

            if (> &y ymax)
                local ymax &y

            local plot-point
                object (. "x" x) (. "y" &y)

            if (in &point "error")
                insert plot-point "error" (&point "error")

            local group
                object
                    . "size"   0.5
                    . "color"  (plot:colors (+ (select single? i j) 2))
                    . "points" (list plot-point)

            if single?
                insert group "label" key

            append (plot "groups") group

            ++ j
            local x (+ x 0.5)

        ++ i
        local x (+ x 0.25)

    local xmax (- x 0.25)
    insert plot "xmax" xmax

    local j    0
    local ypow ymax
    while (> ypow 10)
        local ypow (/ ypow 10)
        ++ j

    local ymaxbase    (+ (math:floor ypow) 1)
    local ymax-padded (* ymaxbase (math:pow 10 j))

    insert plot "ymax"   ymax-padded
    insert plot "ymarks" ymaxbase

    if (not single?)
        if (not (in KWDARGS "legendx"))
            local legendx -0.1
        if (not (in KWDARGS "legendy"))
            local legendy (- 0 (* 0.05 ymax))

        local j 0
        foreach y-key keys
            append (plot "groups")
                object
                    . "label"  y-key
                    . "labelx" (* legendx xmax)
                    . "labely" (- legendy (* j (* 0.05 ymax)))
                    . "color"  (plot:colors (+ 2 j))
            ++ j

    plot
