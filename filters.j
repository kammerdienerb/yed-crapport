fn (date-before month day)
    or (< ((date:parse-iso (@row "date")) "month") month)
       (< ((date:parse-iso (@row "date")) "day")   day)

# @filter
#     and
#         == 2024 ((date:parse-iso (@row "date")) "year")
#         not (date-before 1 13)
#     and
#         != 14 (@row "cores")
#         == nil (@row "fair_weight_regex")

# @filter
#     or
#         (< ((date:parse-iso (@row "date")) "month") 10)
#         (< ((date:parse-iso (@row "date")) "day")   5)
