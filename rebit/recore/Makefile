# melonDS 1.x core + the Recore frontend as one static archive, for Recore's build
# (recore/recipes/melonds1x.json): emmake make -f rebit/recore/Makefile
# The JIT with its WebAssembly back end (or the interpreter, by option), software renderer,
# single thread.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SRC := $(ROOT)/src
OUT ?= $(ROOT)/build-recore
ARCHIVE ?= melonds_recore.a

AR ?= emar
CC ?= emcc
CXX ?= em++

MELONDS_VERSION := 1.1

CORE_SOURCES := \
	ARCodeFile.cpp ARDatabaseDAT.cpp AREngine.cpp ARM.cpp ARMInterpreter.cpp ARMInterpreter_ALU.cpp \
	ARMInterpreter_Branch.cpp ARMInterpreter_LoadStore.cpp CP15.cpp CRC32.cpp DMA.cpp DMA_Timings.cpp \
	DSi.cpp DSi_AES.cpp DSi_Camera.cpp DSi_DSP.cpp DSi_I2C.cpp DSi_I2S.cpp DSi_NAND.cpp DSi_NDMA.cpp \
	DSi_NWifi.cpp DSi_SD.cpp DSi_SPI_TSC.cpp FATIO.cpp FATStorage.cpp GBACart.cpp GBACartMotionPak.cpp \
	GPU.cpp GPU_Soft.cpp GPU2D.cpp GPU2D_Soft.cpp GPU3D.cpp GPU3D_Soft.cpp GPU3D_Texcache.cpp Mic.cpp \
	NDS.cpp NDSCart.cpp ROMList.cpp FreeBIOS.cpp RTC.cpp Savestate.cpp SPI.cpp SPI_Firmware.cpp SPU.cpp \
	Utils.cpp Wifi.cpp WifiAP.cpp \
	ARMJIT.cpp ARMJIT_Memory.cpp ARM_InstrInfo.cpp ARMJIT_WASM/ARMJIT_Compiler.cpp ARMJIT_WASM/ARMJIT_ALU.cpp \
	DSP_HLE/UcodeBase.cpp DSP_HLE/AACUcode.cpp DSP_HLE/G711Ucode.cpp DSP_HLE/GraphicsUcode.cpp \
	NDSCart/CartCommon.cpp NDSCart/CartRetail.cpp NDSCart/CartRetailNAND.cpp NDSCart/CartRetailIR.cpp \
	NDSCart/CartRetailBT.cpp NDSCart/CartSD.cpp NDSCart/CartHomebrew.cpp NDSCart/CartR4.cpp \
	teakra/src/ahbm.cpp teakra/src/apbp.cpp teakra/src/btdmp.cpp teakra/src/disassembler.cpp \
	teakra/src/dma.cpp teakra/src/timer.cpp teakra/src/memory_interface.cpp teakra/src/mmio.cpp \
	teakra/src/parser.cpp teakra/src/processor.cpp teakra/src/teakra.cpp
CORE_C_SOURCES := fatfs/ff.c fatfs/ffsystem.c fatfs/ffunicode.c sha1/sha1.c tiny-AES-c/aes.c \
	xxhash/xxhash.c blip-buf/blip_buf.c
FRONTEND_SOURCES := recore_frontend.cpp platform.cpp

OBJECTS := $(CORE_SOURCES:%.cpp=$(OUT)/core/%.o) $(CORE_C_SOURCES:%.c=$(OUT)/core/%.o) \
	$(FRONTEND_SOURCES:%.cpp=$(OUT)/frontend/%.o)

INCLUDES := -I$(SRC) -I$(SRC)/teakra/include -I$(ROOT)/rebit/recore -I$(OUT)
# the JIT with the WebAssembly back end (src/ARMJIT_WASM), DS only
COMMON_FLAGS := -fwrapv -DJIT_ENABLED -DREBIT_JIT_NDS_ONLY $(INCLUDES) $(RECORE_DEFINES)
override CXXFLAGS += -std=c++17 -Wno-invalid-offsetof -MMD -MP $(COMMON_FLAGS)
override CFLAGS += -MMD -MP $(COMMON_FLAGS)

all: $(ARCHIVE)

$(OUT)/version.h: $(SRC)/version.h.in
	@mkdir -p $(dir $@)
	sed -e 's|$${melonDS_HOMEPAGE_URL}|https://melonds.kuribo64.net|' -e 's|$${melonDS_VERSION}|$(MELONDS_VERSION)|' \
	    -e 's|$${MELONDS_VERSION_SUFFIX}||' $< > $@

$(OUT)/core/%.o: $(SRC)/%.cpp $(OUT)/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/core/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/frontend/%.o: $(ROOT)/rebit/recore/%.cpp $(OUT)/version.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ARCHIVE): $(OBJECTS)
	rm -f $@
	$(AR) rcs $@ $(OBJECTS)

# incremental builds: header dependencies, and a rebuild when the flags change
-include $(OBJECTS:.o=.d)
FLAGS_STAMP := $(OUT)/flags-$(shell echo '$(CXXFLAGS) $(CFLAGS)' | md5sum | cut -c1-12)
$(FLAGS_STAMP):
	@mkdir -p $(OUT) && rm -f $(OUT)/flags-* && touch $@
$(OBJECTS): $(FLAGS_STAMP)

clean:
	rm -rf $(OUT) $(ARCHIVE)

.PHONY: all clean
