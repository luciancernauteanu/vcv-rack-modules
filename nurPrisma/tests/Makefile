# Offline tests for the DSP code. Does not need Rack or RACK_DIR.
CXX ?= g++
FLAGS = -std=c++11 -O2 -Wall -Wextra -I../src

test_prisma: test_prisma.cpp $(wildcard ../src/*.hpp)
	$(CXX) $(FLAGS) -o $@ test_prisma.cpp

run: test_prisma
	./test_prisma

clean:
	rm -f test_prisma test_prisma.exe

.PHONY: run clean
