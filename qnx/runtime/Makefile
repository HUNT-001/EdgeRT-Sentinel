# Runtime services.
#   QNX target : source ~/qnx800/qnxsdp-env.sh && make
#   Host check : make HOST=1        (Linux g++, Unix-socket transport, logic test only)

QCXX     ?= q++
HOST_CXX ?= g++
QVARIANT ?= gcc_ntox86_64
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra
INC      := -I../common/include -I../../accelerator/include

ifeq ($(HOST),1)
  CXXFLAGS += -Werror
  LDLIBS    := -lpthread
  BUILD_CXX := $(HOST_CXX)
  OUT       := ../build/host
else
  BUILD_CXX := $(QCXX) -V$(QVARIANT)
  LDLIBS    :=
  OUT       := ../build/qnx-x86_64
endif

COMMON := src/ipc_qnx.cpp src/ipc_host.cpp ../../accelerator/src/risk_core.cpp
BINS   := $(OUT)/riskd $(OUT)/ipc_test $(OUT)/actuatord $(OUT)/safetyd $(OUT)/motiond $(OUT)/sensor_replayd

.PHONY: all clean
all: $(BINS)

$(OUT)/%: src/%.cpp $(COMMON) $(wildcard src/*.h) $(wildcard ../common/include/*.h) | $(OUT)
	$(BUILD_CXX) $(CXXFLAGS) $(INC) -o $@ $< $(COMMON) $(LDLIBS)

$(OUT):
	mkdir -p $@

clean:
	rm -rf ../build
