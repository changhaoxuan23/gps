ifdef CUDA_HOME
	NVCC ?= $(CUDA_HOME)/bin/nvcc
else
	NVCC ?= nvcc
endif

CXX_FLAGS += -std=c++17 -I.
LD_FLAGS += -lnvidia-ml
SRCS = $(wildcard *.cc)
OBJS = $(SRCS:.cc=.o)
BINS = gps glaunch

all: pre-check release
debug: pre-check
	@CXX_FLAGS="-g" make build
release:
	@CXX_FLAGS="-O3 -DNDEBUG" make build
	strip -s $(BINS)
pre-check:
	@$(NVCC) --version
build: $(OBJS) $(BINS)
gps: gps.o nvml_common.o
	$(NVCC) -o $@ $(LD_FLAGS) $^
glaunch: glaunch.o nvml_common.o
	$(NVCC) -o $@ $(LD_FLAGS) $^
clean:
	rm -f $(BINS) $(OBJS)
get-binaries:
	@echo $(BINS)
%.o: %.cc
	$(NVCC) -c $(CXX_FLAGS) $<
.SUFFIXES:
.PHONY: pre-check all debug release build clean get-binaries