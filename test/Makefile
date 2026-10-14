CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Isrc -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all
LDLIBS = -lm
SOURCES = $(wildcard src/*.c)
.PHONY: test clean
test:
	@test -n "$(SOURCES)" || { echo 'No C source files. Create src/main.c before building.'; exit 1; }
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOURCES) -o build/app $(LDLIBS)
	./build/app --self-test < /dev/null
clean:
	rm -rf build
