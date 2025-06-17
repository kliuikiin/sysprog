GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant
GCC_FLAGS_HH = -Wextra -Werror -Wall -Wno-gnu-folding-constant -ldl -rdynamic

all: test

test:
	gcc $(GCC_FLAGS) userfs.c test.c ../utils/unit.c -I ../utils -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

PHONY: clean

HH:
	gcc $(GCC_FLAGS_HH) userfs.c test.c ../utils/heap_help/heap_help.c ../utils/unit.c -I ../utils -o test

clean:
	rm -f test