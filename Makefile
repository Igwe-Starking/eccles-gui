# eccles_gui - developer makefile (host). the library itself is just src/eccles_gui.c + src/eccles_gui.h + config/eccles_gui_config.h
#
#   make test          unit + invariant tests with the default config
#   make test-asan     the same under AddressSanitizer + UBSan (this is the one CI cares about)
#   make test-all      the whole configuration matrix, every one under the sanitizers
#   make fuzz          300 seeds of the widget fuzzer (a few seconds)
#   make gen-check     fail if the generated tables in src/eccles_gui.c are stale
#   make mocks         compile check the platform paths (esp-idf, pico, stm32, zephyr) with ECCLES_GUI_EXEC_IRAM=1
#   make cxx           the header from C++
#   make tools-test    run the asset converters on sample data
#   make examples     builds + runs every host example (quickstart, demo, meshes, phone)
#   make demo          renders examples/host/demo.c -> build/demo.png (needs python3 + pillow)
#   make phone         renders the 750x1334 phone example -> build/phone.png
#   make bench         timing of the demo scene on this machine
#   make size          code size for a few configurations
#   make clean

CC      ?= gcc
CXX     ?= g++
BUILD   := build
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -O2
INC     := -Isrc -Iconfig
TESTCC  := $(CC) -std=gnu99 -g -O1 -Wall -Wextra $(INC)
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined

# name:flags pairs for the matrix
MATRIX := \
  default: \
  no3d:-DECCLES_GUI_NO_3D \
  noanim:-DECCLES_GUI_NO_ANIM \
  noaa:-DECCLES_GUI_NO_AA \
  noshadow:-DECCLES_GUI_NO_SHADOW \
  nowidgets:-DECCLES_GUI_NO_WIDGETS+-DECCLES_GUI_MAX_VIEWS3D=0 \
  nomeshes:-DECCLES_GUI_NO_BUILTIN_MESHES \
  swap:-DECCLES_GUI_SWAP_BYTES=1 \
  noz:-DECCLES_GUI_3D_ZBUFFER=0+-DECCLES_GUI_3D_TEXTURES=0 \
  tiny:-DECCLES_GUI_MAX_WIDGETS=16+-DECCLES_GUI_MAX_STYLES=8+-DECCLES_GUI_MAX_DIRTY=1+-DECCLES_GUI_MAX_ANIMS=2+-DECCLES_GUI_MAX_VIEWS3D=1+-DECCLES_GUI_3D_MAX_OBJECTS=1+-DECCLES_GUI_3D_MAX_VERTS=48+-DECCLES_GUI_3D_MAX_FACES=64+-DECCLES_GUI_MAX_FONTS=4 \
  big:-DECCLES_GUI_MAX_WIDGETS=400+-DECCLES_GUI_MAX_DIRTY=32+-DECCLES_GUI_MAX_ANIMS=64+-DECCLES_GUI_3D_MAX_OBJECTS=8+-DECCLES_GUI_3D_MAX_VERTS=1000+-DECCLES_GUI_3D_MAX_FACES=1000 \
  lean:-DECCLES_GUI_NO_3D+-DECCLES_GUI_NO_ANIM+-DECCLES_GUI_NO_AA+-DECCLES_GUI_NO_SHADOW

.PHONY: all test test-asan test-all fuzz gen-check mocks cxx tools-test demo phone bench size lint examples clean

all: test-asan

$(BUILD):
	@mkdir -p $(BUILD)

lint: | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c src/eccles_gui.c -o $(BUILD)/lint.o
	@echo "lint ok (-Wall -Wextra -Wpedantic -Wshadow -Wconversion, c99)"

test: | $(BUILD)
	$(TESTCC) tests/test_gui.c src/eccles_gui.c -lm -o $(BUILD)/test_gui
	$(BUILD)/test_gui

test-asan: lint | $(BUILD)
	$(TESTCC) $(SAN) tests/test_gui.c src/eccles_gui.c -lm -o $(BUILD)/test_gui_asan
	$(BUILD)/test_gui_asan

test-all: | $(BUILD)
	@set -e; for e in $(MATRIX); do \
	  n=$${e%%:*}; f=$$(echo $${e#*:} | tr '+' ' '); \
	  echo "== $$n: $$f"; \
	  $(TESTCC) $(SAN) $$f tests/test_gui.c src/eccles_gui.c -lm -o $(BUILD)/test_$$n; \
	  $(BUILD)/test_$$n | tail -1; \
	done

fuzz: | $(BUILD)
	$(TESTCC) -O2 -DFUZZ_SEEDS=300 $(SAN) tests/test_gui.c src/eccles_gui.c -lm -o $(BUILD)/test_fuzz
	$(BUILD)/test_fuzz | grep -E "FAIL|differs|fuzz steps|checks,"

gen-check:
	python3 tools/gen_data.py --check
	python3 tools/gen_api_doc.py --check
	python3 tools/gen_keywords.py --check

mocks: | $(BUILD)
	$(CC) -std=c99 -Wall -Wextra $(INC) -DECCLES_GUI_EXEC_IRAM=1 -DESP_PLATFORM -Itests/mocks/esp_idf -c src/eccles_gui.c -o $(BUILD)/esp.o
	$(CC) -std=c99 -Wall -Wextra $(INC) -DECCLES_GUI_EXEC_IRAM=1 -DPICO_BUILD -c src/eccles_gui.c -o $(BUILD)/pico.o
	$(CC) -std=c99 -Wall -Wextra $(INC) -DECCLES_GUI_EXEC_IRAM=1 -DUSE_HAL_DRIVER -c src/eccles_gui.c -o $(BUILD)/stm32.o
	$(CC) -std=c99 -Wall -Wextra $(INC) -DECCLES_GUI_EXEC_IRAM=1 -D__ZEPHYR__ -Itests/mocks/zephyr -c src/eccles_gui.c -o $(BUILD)/zephyr.o
	$(CC) -std=c99 -Wall -Wextra $(INC) -DECCLES_GUI_PLATFORM=0 -c src/eccles_gui.c -o $(BUILD)/bare.o
	@echo "platform paths compile"

cxx: | $(BUILD)
	$(CC) -std=c99 $(INC) -c src/eccles_gui.c -o $(BUILD)/g.o
	$(CXX) -std=c++11 -Wall -Wextra $(INC) tests/test_cxx.cpp $(BUILD)/g.o -o $(BUILD)/test_cxx
	$(BUILD)/test_cxx

tools-test: | $(BUILD)
	sh tests/test_tools.sh

demo: | $(BUILD)
	$(CC) -std=c99 -O2 $(INC) examples/host/demo.c src/eccles_gui.c -o $(BUILD)/demo
	$(BUILD)/demo $(BUILD)/demo.ppm
	python3 tools/ppm2png.py $(BUILD)/demo.ppm $(BUILD)/demo.png 2

phone: | $(BUILD)
	$(CC) -std=c99 -O2 $(INC) -Iexamples/eccles_phone -Iexamples/eccles_phone/host_stubs examples/eccles_phone/host_sim.c examples/eccles_phone/eccles_phone_ui.c examples/eccles_phone/eccles_gui_port_phone.c src/eccles_gui.c -o $(BUILD)/phone
	$(BUILD)/phone $(BUILD)/phone.ppm
	python3 tools/ppm2png.py $(BUILD)/phone.ppm $(BUILD)/phone.png 1
	python3 tools/ppm2png.py $(BUILD)/phone_shade.ppm $(BUILD)/phone_shade.png 1

examples: demo phone | $(BUILD)
	$(CC) -std=c99 -O2 -Wall -Wextra $(INC) examples/host/quickstart.c src/eccles_gui.c -o $(BUILD)/quickstart
	$(BUILD)/quickstart $(BUILD)/quickstart.ppm
	$(CC) -std=c99 -O2 -Wall -Wextra $(INC) examples/host/meshes.c src/eccles_gui.c -o $(BUILD)/meshes
	$(BUILD)/meshes $(BUILD)/meshes.ppm

bench: | $(BUILD)
	$(CC) -std=c99 -O2 $(INC) examples/host/bench.c src/eccles_gui.c -o $(BUILD)/bench
	$(BUILD)/bench

size: | $(BUILD)
	@for f in "" "-DECCLES_GUI_NO_3D" "-DECCLES_GUI_NO_3D -DECCLES_GUI_NO_ANIM -DECCLES_GUI_NO_SHADOW -DECCLES_GUI_NO_AA" "-DECCLES_GUI_NO_WIDGETS -DECCLES_GUI_MAX_VIEWS3D=0"; do \
	  $(CC) -std=c99 -Os -ffunction-sections -fdata-sections $(INC) $$f -c src/eccles_gui.c -o $(BUILD)/s.o; \
	  echo "[$$f]"; size $(BUILD)/s.o | tail -1; \
	done

clean:
	rm -rf $(BUILD)
