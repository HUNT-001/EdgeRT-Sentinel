# Gate 1 probes.
#   QNX target : source ~/qnx800/qnxsdp-env.sh && make
#   Host check : make HOST=1        (Linux g++, logic test only)

QCXX     ?= q++
HOST_CXX ?= g++
QVARIANT ?= gcc_ntox86_64
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra

ifeq ($(HOST),1)
  BUILD_CXX := $(HOST_CXX)
  LDLIBS    := -lpthread
  OUT       := ../build/host
else
  BUILD_CXX := $(QCXX) -V$(QVARIANT)
  LDLIBS    := -lsocket
  OUT       := ../build/qnx-x86_64
endif

BINS := $(OUT)/rt_probe $(OUT)/link_probe

.PHONY: all clean
all: $(BINS)

$(OUT)/%: %.cpp | $(OUT)
	$(BUILD_CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

$(OUT):
	mkdir -p $@

clean:
	rm -rf ../build
