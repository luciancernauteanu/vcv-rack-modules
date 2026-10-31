# Path to the Rack SDK. Override with `export RACK_DIR=...` or `make RACK_DIR=...`.
RACK_DIR ?= ../Rack-SDK-2.6.6-win-x64/Rack-SDK

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

include $(RACK_DIR)/plugin.mk
