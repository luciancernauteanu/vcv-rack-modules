# Offline tests for the analysis and DSP code. Does not need Rack or RACK_DIR.
CXX ?= g++
FLAGS = -std=c++11 -O2 -Wall -Wextra -I../src

test_bank: test_bank.cpp ../src/WavetableBank.cpp $(wildcard ../src/*.hpp)
	$(CXX) $(FLAGS) -o $@ test_bank.cpp ../src/WavetableBank.cpp

test_engine: test_engine.cpp $(wildcard ../src/*.hpp)
	$(CXX) $(FLAGS) -o $@ test_engine.cpp

test_lfo: test_lfo.cpp $(wildcard ../src/*.hpp)
	$(CXX) $(FLAGS) -o $@ test_lfo.cpp

test_sound: test_sound.cpp ../src/WavetableBank.cpp $(wildcard ../src/*.hpp)
	$(CXX) $(FLAGS) -o $@ test_sound.cpp ../src/WavetableBank.cpp

run: test_bank test_engine test_lfo test_sound
	./test_bank
	./test_engine
	./test_lfo
	./test_sound

clean:
	rm -f test_bank test_bank.exe test_engine test_engine.exe test_lfo test_lfo.exe test_sound test_sound.exe

.PHONY: run clean
