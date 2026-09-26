BUILD_DIR ?= build
BUILD_TYPE ?= RelWithDebInfo
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all configure build run clean distclean format

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

run: build
	./$(BUILD_DIR)/bowsers_image_viewer $(ARGS)

clean:
	cmake --build $(BUILD_DIR) --target clean || true

distclean:
	rm -rf $(BUILD_DIR)

format:
	find src include -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i
