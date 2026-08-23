# Primo ARM Miner Makefile

PRIMO_LINKER ?= lld

ifeq ($(origin CC), default)
CC = clang-16
endif

ifeq ($(origin CXX), default)
CXX = clang++-16
endif

CC_VERSION_LINE := $(shell $(CC) --version 2>/dev/null | head -1 | tr '[:upper:]' '[:lower:]')
CC_IS_CLANG := $(findstring clang,$(CC_VERSION_LINE))

# Device profile selector. The CLHash hand-asm is now runtime-dispatched
# (compiled into both _asm/_noasm variants and selected per core — see
# clhash_native.h), so the profile NO LONGER gates the asm. It only toggles the
# Cortex-A53 erratum workaround:
#   rk3588  (default) — adds -mfix-cortex-a53-835769 (a harmless NOP elsewhere).
#   generic           — drops it (phones/SBCs with no real A53 core):
#                         make PROFILE=generic
# Both produce the same runtime-dispatched binary otherwise; -mtune=cortex-a53
# is kept in both (it generalises better than -mtune=native even on the A76).
# PRIMO_A53_ERRATA=0/1 toggles the erratum independently of PROFILE.
PROFILE ?= rk3588

COMMON_CPPFLAGS = -flax-vector-conversions -I./include -I./src -I./third_party/yespower
DEPFLAGS = -MMD -MP
COMMON_OPT_FLAGS = -O3 -ffinite-loops -ffast-math
COMMON_OPT_FLAGS += -D_REENTRANT -DUSE_DIRECT_NATIVE_CALL=1
COMMON_OPT_FLAGS += -falign-functions=16 -fomit-frame-pointer -fpic
COMMON_OPT_FLAGS += -pthread -flto -fno-stack-protector -Wall

# ISA and scheduling baseline. These default to the validated winner
# (-march=armv8-a+crypto, -mtune=cortex-a53 — see notes below) but are exposed
# as overridable knobs so CI can A/B build flags per device WITHOUT editing the
# Makefile. `?=` means an env var or a `make PRIMO_MARCH=...` arg wins; the
# default is unchanged when nothing is passed. Do NOT default-bump these to
# armv8.2-a (LSE atomics SIGILL on ARMv8.0 cores — see the Termux build).
PRIMO_MARCH ?= armv8-a+crypto
PRIMO_MTUNE ?= cortex-a53

ifeq ($(PROFILE),generic)
# Portable profile for non-A76 SBCs. The CLHash hand-asm is no longer gated by
# the profile: it is compiled into BOTH builds (clhash_native.c = _asm,
# clhash_native_noasm.c = _noasm) and selected per-thread at runtime by core
# type (see clhash_native.h), so one binary is optimal on every core. The a53
# tune is kept on purpose (bench data: conservative -mtune=cortex-a53
# generalises better than -mtune=native even on the A76). The a53 errata
# workaround is dropped here (not an A53).
BASE_ARCH_FLAGS = -march=$(PRIMO_MARCH) -mtune=$(PRIMO_MTUNE)
else
# Match the promoted ccminer baseline profile on this device.
# The validated winner tuned for A55/A53-style codegen, not the earlier A76 profile.
BASE_ARCH_FLAGS = -march=$(PRIMO_MARCH) -mtune=$(PRIMO_MTUNE)
# A53 erratum 835769 workaround. On by default for the rk3588 SBC profile;
# Android/phone builds (no real A53) pass PRIMO_A53_ERRATA=0 to drop the NOP
# overhead without losing the rk3588 A76 asm helpers (which help even on the
# Mongoose M4 — so phones keep this profile rather than switching to generic).
ifneq ($(PRIMO_A53_ERRATA),0)
COMMON_OPT_FLAGS += -mfix-cortex-a53-835769
endif
endif

# Last-resort full replacement of the arch/tune flags for a one-off CI probe.
ifneq ($(strip $(PRIMO_ARCH_FLAGS_OVERRIDE)),)
BASE_ARCH_FLAGS = $(PRIMO_ARCH_FLAGS_OVERRIDE)
endif

# Additive per-build extra flags (empty by default — append, never replace).
PRIMO_CPPFLAGS = $(COMMON_CPPFLAGS) $(PRIMO_EXTRA_CPPFLAGS)
PRIMO_CFLAGS = $(BASE_ARCH_FLAGS) $(COMMON_OPT_FLAGS) -std=gnu11 $(PRIMO_EXTRA_CFLAGS)
PRIMO_CXXFLAGS = $(BASE_ARCH_FLAGS) $(COMMON_OPT_FLAGS) -funroll-loops -std=c++14 $(PRIMO_EXTRA_CXXFLAGS)
PRIMO_LDFLAGS = -flto -pthread $(PRIMO_EXTRA_LDFLAGS)
PRIMO_LDLIBS = -lcurl -ljansson -lm
ifneq ($(strip $(PRIMO_LDLIBS_OVERRIDE)),)
PRIMO_LDLIBS = $(PRIMO_LDLIBS_OVERRIDE)
endif

# --- RandomX (Monero) — vendored reference library (third_party/RandomX) -----
# Built via its own CMake with ITS OWN conservative flags, NOT ours:
#  - our -ffast-math would MISCOMPILE it (RandomX floating point is
#    consensus-critical IEEE-754; wrong rounding = wrong hashes),
#  - never ARCH=native (armv8.2 LSE = SIGILL on ARMv8.0 — the Termux lesson).
# Its default aarch64 build is exactly our baseline (-march=armv8-a+crypto,
# hardware AES selected at runtime). PRIMO_RANDOMX=0 builds the miner without
# RandomX (drops the cmake build dependency).
PRIMO_RANDOMX ?= 1
RANDOMX_DIR = third_party/RandomX
RANDOMX_BUILD_DIR = $(RANDOMX_DIR)/build
RANDOMX_LIB = $(RANDOMX_BUILD_DIR)/librandomx.a
ifneq ($(PRIMO_RANDOMX),0)
PRIMO_CPPFLAGS += -DPRIMO_RANDOMX=1 -I$(RANDOMX_DIR)/src
RANDOMX_LINK = $(RANDOMX_LIB)
else
RANDOMX_LINK =
endif

ifneq ($(CC_IS_CLANG),)
PRIMO_CFLAGS += -mllvm -enable-loop-distribute
SCRYPT_NOSLP_FLAG = -fno-slp-vectorize
else
SCRYPT_NOSLP_FLAG = -fno-tree-slp-vectorize
endif

ifneq ($(strip $(PRIMO_LINKER)),)
PRIMO_LDFLAGS += -fuse-ld=$(PRIMO_LINKER)
endif

CPPFLAGS += $(PRIMO_CPPFLAGS)
CFLAGS += $(PRIMO_CFLAGS)
CXXFLAGS += $(PRIMO_CXXFLAGS)
LDFLAGS += $(PRIMO_LDFLAGS)
LDLIBS += $(PRIMO_LDLIBS)

# Optional per-file override for CLHash experiments, e.g.
# make CLHASH_EXTRA_FLAGS="-DCLHASH_ASM_CASE18_MASK_PTRS=0"
CLHASH_EXTRA_FLAGS ?=

SOURCES_C = \
	src/utils/dns_fallback.c \
	src/algorithm/clhash_native.c \
	src/algorithm/clhash_native_noasm.c \
	src/algorithm/haraka_native.c \
	src/algorithm/cpu_features.c \
	src/algorithm/scrypt_neon.c \
	src/algorithm/sha256_neon.c \
	third_party/yespower/yespower-ref.c \
	third_party/yespower/sha256.c

SOURCES_CPP = \
	src/main.cpp \
	src/config.cpp \
	src/api.cpp \
	src/miner.cpp \
	src/dev_fee.cpp \
	src/stratum.cpp \
	src/stratum_state.cpp \
	src/stratum_job.cpp \
	src/stratum_rpc.cpp \
	src/stratum_handshake.cpp \
	src/stratum_transport.cpp \
	src/stratum_session.cpp \
	src/stratum_standard.cpp \
	src/stratum_verus.cpp \
	src/algorithm/verus.cpp \
	src/algorithm/civiclight.cpp \
	src/utils/log.cpp

ifneq ($(PRIMO_RANDOMX),0)
SOURCES_CPP += src/algorithm/randomx_algo.cpp \
	src/stratum_xmr.cpp
endif

SOURCES_ASM = \
	src/algorithm/sha256_ce_asm.S \
	src/algorithm/scrypt_blockmix_asm.S

OBJECTS = $(SOURCES_C:.c=.o) $(SOURCES_CPP:.cpp=.o) $(SOURCES_ASM:.S=.o)

TARGET = ghuleh-miner

.PHONY: all clean test

all: $(TARGET)

# Repo test harness: per-algo init self-tests + verus x2/fused/asm runtime
# cross-check + end-to-end share round-trips against a local mock stratum
# pool (plain TCP and, when libcurl has TLS, stratum+ssl). ~45 s.
test: $(TARGET)
	@bash tests/run_tests.sh ./$(TARGET)

$(TARGET): $(OBJECTS) $(RANDOMX_LINK)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) $(RANDOMX_LINK) $(LDFLAGS) $(LDLIBS) -o $(TARGET)
	@echo "Build complete!"
	@ls -lh $(TARGET)

# One-time cmake build of the vendored library (see PRIMO_RANDOMX notes above).
# Not removed by `clean` — `make randomx-clean` rebuilds it from scratch.
$(RANDOMX_LIB):
	@echo "Building vendored RandomX library (one-time)..."
	cmake -S $(RANDOMX_DIR) -B $(RANDOMX_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=$(CC) -DCMAKE_CXX_COMPILER=$(CXX) >/dev/null
	cmake --build $(RANDOMX_BUILD_DIR) --target randomx -j $(shell nproc 2>/dev/null || echo 4)

.PHONY: randomx-clean
randomx-clean:
	rm -rf $(RANDOMX_BUILD_DIR)

# Scrypt needs -fno-slp-vectorize: the in-place XOR loops get SLP-vectorized
# to NEON, causing NEON→scalar store forwarding penalty when scalar Salsa reads B
src/algorithm/scrypt_neon.o: src/algorithm/scrypt_neon.c
	@echo "Compiling $< (no SLP vectorize)..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) $(SCRYPT_NOSLP_FLAG) -c $< -o $@

# Verus hot-path C files: match ccminer's exact compile environment.
# -fno-unroll-loops: ccminer CFLAGS omits -funroll-loops for C files; these
#   hand-tuned NEON/PMULL/AES intrinsics benefit from no extra unrolling.
# Global CPPFLAGS already provide:
#   -flax-vector-conversions
# NOTE: -fno-strict-aliasing is intentionally NOT set — its removal gives
#   +1.1-1.2% on Verus (CLHash mixed pointer types benefit from strict
#   aliasing letting the compiler reorder loads/stores). See CLAUDE.md.
src/algorithm/clhash_native.o: src/algorithm/clhash_native.c
	@echo "Compiling $< (no unroll-loops, asm variant)..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -fno-unroll-loops $(CLHASH_EXTRA_FLAGS) -c $< -o $@

# No-asm variant of the same hot loop (wrapper #includes clhash_native.c with
# CLHASH_SYM_SUFFIX=_noasm + CLHASH_ASM_*=0). Same -fno-unroll-loops as above.
src/algorithm/clhash_native_noasm.o: src/algorithm/clhash_native_noasm.c src/algorithm/clhash_native.c
	@echo "Compiling $< (no unroll-loops, no-asm variant)..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -fno-unroll-loops $(CLHASH_EXTRA_FLAGS) -c $< -o $@

src/algorithm/haraka_native.o: src/algorithm/haraka_native.c
	@echo "Compiling $< (no unroll-loops)..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -fno-unroll-loops -c $< -o $@

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(TARGET)
	@echo "Clean complete"

-include $(OBJECTS:.o=.d)
