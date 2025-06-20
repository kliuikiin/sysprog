GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all:
	gcc $(GCC_FLAGS) solution.c parser.c ../utils/heap_help/heap_help.c -I ../utils/heap_help -ldl -o a.out
#gcc $(GCC_FLAGS) solution.c parser.c -o a.out

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) solution.c parser.c ../utils/heap_help/heap_help.c -I ../utils/heap_help -ldl -o mybash