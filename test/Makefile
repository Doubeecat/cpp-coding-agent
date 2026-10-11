CXX = c++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Isrc -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all
SOURCES = $(wildcard src/*.cpp)
.PHONY: test clean
test:
	@test -n "$(SOURCES)" || { echo 'No C++ source files. Create src/main.cpp before building.'; exit 1; }
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o build/app
# 	./build/app --self-test < /dev/null
clean:
	rm -rf build
