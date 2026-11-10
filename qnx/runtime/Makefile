# Runtime services.
#   QNX target : source ~/qnx800/qnxsdp-env.sh && make
#   Host check : make HOST=1        (Linux g++, Unix-socket transport, logic test only)

QCXX     ?= q++
QVARIANT ?= gcc_ntox86_64
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra
INC      := -I../common/include -I../../accelerator/include

ifeq ($(HOST),1)
  CXXFLAGS += -Werror
  BUILD_CXX := $(CXX)
  OUT       := ../build/host
else
  BUILD_CXX := $(QCXX) -V$(QVARIANT)
  OUT       := ../build/qnx-x86_64
endif

COMMON := src/ipc_qnx.cpp src/ipc_host.cpp ../../accelerator/src/risk_core.cpp
BINS   := $(OUT)/riskd $(OUT)/ipc_test

.PHONY: all clean
all: $(BINS)

$(OUT)/%: src/%.cpp $(COMMON) | $(OUT)
	$(BUILD_CXX) $(CXXFLAGS) $(INC) -o $@ $< $(COMMON)

$(OUT):
	mkdir -p $@

clean:
	rm -rf ../build
