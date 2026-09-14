#
# SCPU-EMU - top-level Makefile
#
#   make tests    build and run the host test suite (needs only g++)
#   make firmware build the Raspberry Pi kernel image (needs a Circle tree,
#                 see Docs/build.md -- delegates to Source/Makefile)
#   make clean
#
# The host build compiles everything that is platform independent: the CPU
# cores, the C64 banking and memory model, the SuperCPU layer, and the host bus
# backend. Source/Bus/RAD and Source/App are excluded -- they need Circle and a
# Raspberry Pi.
#

CXX      ?= g++

# -fno-rtti -fno-exceptions: this is embedded-oriented code that uses neither,
# and the firmware build does not have them either, so the host tests should
# exercise the same code generation. It also sidesteps a silent ld failure
# (exit 116, no diagnostic) when linking weak typeinfo from these headers under
# MSYS2/mingw-w64 GCC 14 -- see Docs/build.md.
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c++14 \
            -fno-rtti -fno-exceptions -DSCPU_HOST_BUILD

BUILDDIR  = build/host

HOST_SRCS = \
	Source/CPU/M6502/m6502.cpp \
	Source/CPU/M6502/m6502_opcodes.cpp \
	Source/CPU/W65C816/w65c816.cpp \
	Source/CPU/W65C816/w65c816_opcodes.cpp \
	Source/C64/banking.cpp \
	Source/C64/c64_memory.cpp \
	Source/SuperCPU/write_buffer.cpp \
	Source/SuperCPU/registers.cpp \
	Source/SuperCPU/supercpu.cpp \
	Source/SuperCPU/fast_ram.cpp \
	Source/SuperCPU/memory_map.cpp \
	Source/Bus/Host/host_bus.cpp \
	Source/Video/vic_raster.cpp \
	Source/Video/vic_renderer.cpp \
	Source/REU/reu.cpp \
	Source/RAMLink/ramlink.cpp

TEST_SRCS = \
	Tests/test_main.cpp \
	Tests/CPU/test_m6502.cpp \
	Tests/CPU/test_w65c816.cpp \
	Tests/CPU/test_w65c816_diff.cpp \
	Tests/C64/test_banking.cpp \
	Tests/Video/test_vic_raster.cpp \
	Tests/Video/test_vic_renderer.cpp \
	Tests/SuperCPU/test_write_buffer.cpp \
	Tests/Integration/test_kernal_boot.cpp \
	Tests/Integration/test_real_kernal.cpp \
	Tests/Integration/test_kernal_65816.cpp \
	Tests/SuperCPU/test_memory_map.cpp \
	Tests/REU/test_reu.cpp \
	Tests/RAMLink/test_ramlink.cpp

HOST_OBJS = $(HOST_SRCS:%.cpp=$(BUILDDIR)/%.o)
TEST_OBJS = $(TEST_SRCS:%.cpp=$(BUILDDIR)/%.o)

TEST_BIN = $(BUILDDIR)/scpu-tests

.PHONY: all tests build-tests check sanitizers firmware clean clean-sanitizers

all: tests

tests: build-tests
	@$(TEST_BIN)

build-tests: $(TEST_BIN)

# Run the normal optimized suite and a second build under AddressSanitizer and
# UndefinedBehaviorSanitizer. A separate output tree prevents a sanitized run
# from contaminating an ordinary incremental build. LeakSanitizer is disabled
# because several supported test environments run under ptrace, where LSan
# cannot operate; allocation lifetime is still exercised by ASan.
SANITIZER_FLAGS = -O1 -g -Wall -Wextra -std=c++14 -fno-rtti -fno-exceptions \
                  -DSCPU_HOST_BUILD -fsanitize=address,undefined \
                  -fno-omit-frame-pointer

check: tests sanitizers

sanitizers:
	@rm -rf build/sanitize
	@$(MAKE) --no-print-directory -j1 BUILDDIR=build/sanitize \
		CXXFLAGS='$(SANITIZER_FLAGS)' build-tests
	@ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=0}" \
		UBSAN_OPTIONS="$${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
		build/sanitize/scpu-tests

clean-sanitizers:
	rm -rf build/sanitize

$(TEST_BIN): $(HOST_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# --- VICE conformance tools -------------------------------------------------
# Host programs that drive the SHIPPING renderer, band planner and memory map
# against a reference emulator. Built from the same objects as the test suite,
# so a fix cannot pass here and behave differently on the card.
#
#   render_state  one machine state -> one frame        (geometry, modes, sprites)
#   replay_state  a running program -> a replayed frame (write log, anchors, bands)
#   sid_trace     a running program -> its bus traffic  (every SID write delivered)
#   ss816         SingleStepTests vectors -> our 65816   (external CPU truth)
#   scpu_trace    CMD's own ROM -> our register layer   (coverage, not semantics)
#
# Tools/viceconf/capture.sh drives xscpu64 and diffs the result.
VICECONF_BIN = $(BUILDDIR)/render_state $(BUILDDIR)/replay_state \
               $(BUILDDIR)/sid_trace $(BUILDDIR)/ss816 \
               $(BUILDDIR)/scpu_trace

.PHONY: viceconf
viceconf: $(VICECONF_BIN)

$(BUILDDIR)/render_state: $(BUILDDIR)/Tools/viceconf/render_state.o $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/replay_state: $(BUILDDIR)/Tools/viceconf/replay_state.o $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/sid_trace: $(BUILDDIR)/Tools/viceconf/sid_trace.o $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/ss816: $(BUILDDIR)/Tools/viceconf/ss816.o $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILDDIR)/scpu_trace: $(BUILDDIR)/Tools/viceconf/scpu_trace.o $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

-include $(HOST_OBJS:.o=.d) $(TEST_OBJS:.o=.d) \
         $(BUILDDIR)/Tools/viceconf/render_state.d \
         $(BUILDDIR)/Tools/viceconf/replay_state.d \
         $(BUILDDIR)/Tools/viceconf/sid_trace.d \
         $(BUILDDIR)/Tools/viceconf/ss816.d \
         $(BUILDDIR)/Tools/viceconf/scpu_trace.d

# The CMD SuperCPU Compendium is NOT in this repository.
#
# It is CMD's documentation, not this project's to redistribute, so the PDF and
# its HTML source were removed from the tree and from history before the
# repository was made public -- along with a VICE snapshot that embedded
# Commodore's BASIC and KERNAL ROMs. There was a `docs-pdf` target here that
# re-rendered the PDF from the HTML; both files are gone, so the target went
# with them rather than being left to fail with a confusing error.
#
# Findings that came OUT of the compendium are quoted, with page references,
# in Docs/SuperCPU64/. Those quotations stay: they are short, attributed, and
# are the evidence behind specific emulation decisions.

# Builds the Raspberry Pi kernel image. Fetches the AArch64 toolchain and
# Circle 44.3 into _toolchain/ on first run, applies RAD's Circle settings, then
# builds. See Docs/build.md.
firmware:
	@bash Tools/setup_buildchain.sh

# Rebuild the firmware only, assuming the chain is already set up.
firmware-quick:
	@bash Tools/setup_buildchain.sh

# Assemble everything that goes on the SD card into SDCard/.
# kernel8.img comes from `make firmware` and is copied in if it exists.
SDDIR = SDCard

# stage_rom <card name> <candidate source names...>
# Copies the first candidate that exists in ROMs/ to SCPU/<card name>, and says
# so. Reports a MISSING line naming every candidate when none is found -- the
# whole point is that an incomplete set is loud rather than silent.
define stage_rom
	src=""; \
	for c in $(1) $(2); do \
		if [ -f "ROMs/$$c" ]; then src="ROMs/$$c"; break; fi; \
	done; \
	if [ -n "$$src" ]; then \
		cp -f "$$src" "$(SDDIR)/SCPU/$(1)"; \
		echo "  $(1)  <- $$src"; \
	else \
		echo "  MISSING $(1)  -- looked for: $(1) $(2)  (see ROMs/README.md)"; \
	fi
endef

.PHONY: sdcard
sdcard:
	@mkdir -p $(SDDIR)/SCPU
	@cp -f Firmware/RaspberryPi/bootcode.bin      $(SDDIR)/ 2>/dev/null || echo "  MISSING bootcode.bin      (see Firmware/RaspberryPi/README.md)"
	@cp -f Firmware/RaspberryPi/start.elf         $(SDDIR)/ 2>/dev/null || echo "  MISSING start.elf         (see Firmware/RaspberryPi/README.md)"
	@cp -f Firmware/RaspberryPi/fixup.dat         $(SDDIR)/ 2>/dev/null || echo "  MISSING fixup.dat         (see Firmware/RaspberryPi/README.md)"
	@# config.txt sets gpu_mem=16, which makes the firmware load the CUT-DOWN
	@# loader instead of start.elf/fixup.dat. Omitting these two reports as
	@# "start.elf not found" (3 ACT-LED flashes) even though start.elf is present.
	@cp -f Firmware/RaspberryPi/start_cd.elf      $(SDDIR)/ 2>/dev/null || echo "  MISSING start_cd.elf      -- REQUIRED, gpu_mem=16 selects it over start.elf"
	@cp -f Firmware/RaspberryPi/fixup_cd.dat      $(SDDIR)/ 2>/dev/null || echo "  MISSING fixup_cd.dat      -- REQUIRED, pairs with start_cd.elf"
	@cp -f Firmware/RaspberryPi/LICENCE.broadcom  $(SDDIR)/ 2>/dev/null || true
	@cp -f Firmware/RaspberryPi/config.txt        $(SDDIR)/
	@cp -f Firmware/ARMSTUB/rad-prefetch.bin      $(SDDIR)/ 2>/dev/null \
		|| echo "  MISSING rad-prefetch.bin  -- run 'make firmware'; config.txt selects it via armstub= and the bus timings depend on it"
	@cp -f Config/default.cfg                     $(SDDIR)/SCPU/scpu.cfg
	@# The card wants four fixed names; a developer's ROMs/ directory usually
	@# holds the canonical dumps under their Commodore part numbers instead.
	@# Resolve either, the same way Source/Common/rom_paths.h does for the host
	@# tools, so `make sdcard` stages a COMPLETE card from an unmodified set of
	@# downloads. Staging three of four files and saying nothing is how a card
	@# reaches a C64 and quietly boots normally.
	@$(call stage_rom,kernal.rom,kernal.901227-03.bin kernal-901227-03.bin 901227-03.bin)
	@$(call stage_rom,basic.rom,basic.901226-01.bin basic-901226-01.bin 901226-01.bin)
	@$(call stage_rom,chargen.rom,characters.901225-01.bin chargen-901225-01.bin chargen-906143-02.bin)
	@# The SuperCPU's own ROM: SuperCPU DOS and its JiffyDOS support. Optional --
	@# the accelerator boots the machine's own KERNAL without it.
	@#
	@# SuperCPU DOS 2.04, confirmed working on this machine. An early debugging
	@# round blamed 2.04 for "INITIALIZATION ERROR: 06" and switched to 1.4; the
	@# real culprits were since-fixed emulation bugs, and 2.04 is preferred.
	@$(call stage_rom,scpu.rom,scpu-dos-2.04.bin scpu-dos-1.4.bin)
	@# The RAMLink EPROM. Optional: only meaningful with RAMLINK_SIZE set, and
	@# its absence is reported at startup rather than stopping the boot.
	@if [ -f ROMs/ramlink.rom ] || [ -f ROMs/ramlink201.bin ] || [ -f ROMs/ramlink140.bin ]; then \
		$(call stage_rom,ramlink.rom,ramlink201.bin ramlink140.bin); \
	else \
		echo "  (no RAMLink ROM staged -- optional, see ROMs/README.md)"; \
	fi
	@# The RAMCard image: a raw dump of the card's RAM.
	@#
	@# The image has to MATCH the card RAMLINK_SIZE fits, not merely fit inside
	@# it. RAMLink keeps its system partition -- device number, disk name and
	@# partition table -- at the TOP of the card, so an 8MB image dropped into
	@# a 16MB card puts that structure in the middle, where RAMLink does not
	@# look, and the card reads as unformatted. Tools/mkramcard.py relocates it.
	@# So the staged image is selected by the configured size rather than by
	@# whichever file happens to exist.
	@rlsize=$$(sed -n 's/^RAMLINK_SIZE[ \t][ \t]*\([0-9][0-9]*\).*/\1/p' Config/default.cfg | tail -1); \
	case "$$rlsize" in \
		6) want="ramlink16.rl"; mb=16 ;; \
		5) want="ramlink.rl";   mb=8  ;; \
		*) want="ramlink.rl";   mb=0  ;; \
	esac; \
	if [ -f ROMs/ramlink.img ]; then \
		cp -f ROMs/ramlink.img $(SDDIR)/SCPU/ramlink.img; \
		echo "  ramlink.img  <- ROMs/ramlink.img"; \
	elif [ -f "ROMs/$$want" ]; then \
		cp -f "ROMs/$$want" $(SDDIR)/SCPU/ramlink.img; \
		echo "  ramlink.img  <- ROMs/$$want  (RAMLINK_SIZE $$rlsize)"; \
		if [ "$$mb" != "0" ]; then \
			have=$$(wc -c < "ROMs/$$want" | tr -d ' '); \
			want_bytes=$$(( mb * 1048576 )); \
			if [ "$$have" != "$$want_bytes" ]; then \
				echo "  WARNING: ROMs/$$want is $$have bytes, a $${mb}MB card is $$want_bytes"; \
				echo "           run: Tools/mkramcard.py ROMs/ramlink.rl ROMs/$$want $$mb"; \
			fi; \
		fi; \
	else \
		echo "  (no RAMCard image staged -- optional, card starts blank)"; \
		if [ -f ROMs/ramlink.rl ]; then \
			echo "   ROMs/ramlink.rl exists but RAMLINK_SIZE $$rlsize wants ROMs/$$want"; \
			echo "   run: Tools/mkramcard.py ROMs/ramlink.rl ROMs/$$want $$mb"; \
		fi; \
	fi
	@cp -f Source/kernel8.img $(SDDIR)/ 2>/dev/null \
		|| echo "  MISSING kernel8.img       -- run 'make firmware' first (needs a Circle tree, see Docs/build.md)"
	@echo ""
	@echo "SD card staged in $(SDDIR)/ :"
	@cd $(SDDIR) && find . -type f | sort | sed 's|^\./|  |'

clean:
	rm -rf build
	-$(MAKE) -C Source clean 2>/dev/null || true

clean-sdcard:
	rm -rf $(SDDIR)
