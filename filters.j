fn (date-before month day)
    or (< ((date:parse-iso (@row "date")) "month") month)
       (< ((date:parse-iso (@row "date")) "day")   day)

# @filter
#     or
#         (< ((date:parse-iso (@row "date")) "month") 10)
#         (< ((date:parse-iso (@row "date")) "day")   5)
