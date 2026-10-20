CC=gcc

CFLAGS=-Wall -Wextra -g -Iinclude

SRC=$(wildcard src/*.c src/*/*.c)

OUTPUT=bin/

.PHONY: all clean check

all: wing-gcc

mk_output_dir:
	mkdir -p $(OUTPUT)

wing-gcc: mk_output_dir
	$(CC) $(CFLAGS) $(SRC) -o $(OUTPUT)$@

lexer_test: mk_output_dir
	$(CC) $(CFLAGS) -DTEST_LEXER \
	src/frontend/lexer.c src/util/diagnostic.c src/util/hashmap.c src/util/source.c src/util/common.c \
	-o $(OUTPUT)$@

hashmap_test: mk_output_dir
	$(CC) $(CFLAGS) -DTEST_HASHMAP \
	src/util/hashmap.c src/util/diagnostic.c src/util/source.c src/util/common.c \
	-o $(OUTPUT)$@

diagnostic_test: mk_output_dir
	$(CC) $(CFLAGS) -DTEST_DIAGNOSTIC \
	src/util/diagnostic.c src/util/source.c src/util/common.c \
	-o $(OUTPUT)$@

source_test: mk_output_dir
	$(CC) $(CFLAGS) -DTEST_SOURCE \
	src/util/source.c src/util/diagnostic.c src/util/common.c \
	-o $(OUTPUT)$@

check: wing-gcc lexer_test diagnostic_test source_test hashmap_test
	./bin/source_test
	./bin/diagnostic_test
	./bin/hashmap_test
	./bin/lexer_test
	./bin/lexer_test tests/tests/basic.c > /dev/null
	./bin/wing-gcc tests/tests/basic.c > /dev/null
	@echo "all checks passed"

clean:
	rm -rf $(OUTPUT)
