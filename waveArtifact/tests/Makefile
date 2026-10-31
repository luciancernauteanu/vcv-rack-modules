# Offline tests for the analysis and DSP code. Does not need Rack or RACK_DIR.
CXX ?= g++
FLAGS = -std=c++11 -O2 -Wall -Wextra -I../src

test_bank: test_bank.cpp ../src/WavetableBank.cpp ../src/WavetableBank.hpp
	$(CXX) $(FLAGS) -o $@ test_bank.cpp ../src/WavetableBank.cpp

test_engine: test_engine.cpp ../src/MorphEngine.hpp ../src/WavetableBank.hpp
	$(CXX) $(FLAGS) -o $@ test_engine.cpp

test_lfo: test_lfo.cpp ../src/Lfo.hpp
	$(CXX) $(FLAGS) -o $@ test_lfo.cpp

run: test_bank test_engine test_lfo
	./test_bank
	./test_engine
	./test_lfo

clean:
	rm -f test_bank test_bank.exe test_engine test_engine.exe test_lfo test_lfo.exe

.PHONY: run clean
