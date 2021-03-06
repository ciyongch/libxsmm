ROOTDIR = $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
DEPDIR = ../..
SRCDIR = .
INCDIR = .
BLDDIR = obj
OUTDIR = .

CXXFLAGS = $(NULL)
CFLAGS = $(NULL)
DFLAGS = $(NULL)

UNUSED = 1
BLAS = 1
OMP = 1
SYM = 1

# include common Makefile artifacts
include $(DEPDIR)/Makefile.inc

# necessary include directories
IFLAGS += -I$(call quote,$(INCDIR))
IFLAGS += -I$(call quote,$(DEPDIR)/include)

OUTNAME := $(shell basename "$(ROOTDIR)")
HEADERS := $(wildcard $(INCDIR)/*.h) $(wildcard $(INCDIR)/*.hpp) $(wildcard $(INCDIR)/*.hxx) $(wildcard $(INCDIR)/*.hh) \
           $(wildcard $(SRCDIR)/*.h) $(wildcard $(SRCDIR)/*.hpp) $(wildcard $(SRCDIR)/*.hxx) $(wildcard $(SRCDIR)/*.hh) \
           $(DEPDIR)/include/libxsmm_source.h
CPPSRCS := $(wildcard $(SRCDIR)/*.cpp)
CXXSRCS := $(wildcard $(SRCDIR)/*.cxx)
CCXSRCS := $(wildcard $(SRCDIR)/*.cc)
CSOURCS := $(wildcard $(SRCDIR)/*.c)
CPPOBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(CPPSRCS:.cpp=-cpp.o)))
CXXOBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(CXXSRCS:.cxx=-cxx.o)))
CCXOBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(CCXSRCS:.cc=-cc.o)))
COBJCTS := $(patsubst %,$(BLDDIR)/%,$(notdir $(CSOURCS:.c=-c.o)))
ifneq (,$(strip $(FC)))
FXXSRCS := $(wildcard $(SRCDIR)/*.f)
F77SRCS := $(wildcard $(SRCDIR)/*.F)
F90SRCS := $(wildcard $(SRCDIR)/*.f90) $(wildcard $(SRCDIR)/*.F90)
FXXOBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(FXXSRCS:.f=-f.o)))
F77OBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(F77SRCS:.F=-f77.o)))
F90OBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(F90SRCS:.f90=-f90.o)))
F90OBJS := $(patsubst %,$(BLDDIR)/%,$(notdir $(F90OBJS:.F90=-f90.o)))
endif
SOURCES := $(CPPSRCS) $(CXXSRCS) $(CCXSRCS) $(CSOURCS)
OBJECTS := $(CPPOBJS) $(CXXOBJS) $(CCXOBJS) $(COBJCTS)
FTNSRCS := $(FXXSRCS) $(F77SRCS) $(F90SRCS)
MODULES := $(addsuffix .mod,$(basename $(FTNSRCS))) $(addsuffix .modmic,$(basename $(FTNSRCS)))
FTNOBJS := $(FXXOBJS) $(F77OBJS) $(F90OBJS)
XFILES := $(OUTDIR)/edge_vol_int \
	$(OUTDIR)/asparse_srsoa_f64 $(OUTDIR)/bsparse_srsoa_f64 \
	$(OUTDIR)/asparse_srsoa_f32 $(OUTDIR)/bsparse_srsoa_f32 \
	$(OUTDIR)/bsparse_scsoa_f64 $(OUTDIR)/bsparse_scsoa_f32 \
	$(OUTDIR)/dense_packedacrm_f64 $(OUTDIR)/dense_packedacrm_f32 \
	$(OUTDIR)/dense_packedbcrm_f64 $(OUTDIR)/dense_packedbcrm_f32

.PHONY: all
all: $(XFILES)

.PHONY: compile
compile: $(OBJECTS) $(FTNOBJS)

$(OUTDIR)/edge_vol_int: $(OUTDIR)/.make $(BLDDIR)/edge_vol_int-c.o $(BLDDIR)/edge_proxy_common-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/edge_vol_int-c.o $(BLDDIR)/edge_proxy_common-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/asparse_srsoa_f64: $(OUTDIR)/.make $(BLDDIR)/asparse_srsoa-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/asparse_srsoa-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/bsparse_srsoa_f64: $(OUTDIR)/.make $(BLDDIR)/bsparse_srsoa-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/bsparse_srsoa-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/asparse_srsoa_f32: $(OUTDIR)/.make $(BLDDIR)/asparse_srsoa-c-f32.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/asparse_srsoa-c-f32.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/bsparse_srsoa_f32: $(OUTDIR)/.make $(BLDDIR)/bsparse_srsoa-c-f32.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/bsparse_srsoa-c-f32.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/bsparse_scsoa_f64: $(OUTDIR)/.make $(BLDDIR)/bsparse_scsoa-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/bsparse_scsoa-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/bsparse_scsoa_f32: $(OUTDIR)/.make $(BLDDIR)/bsparse_scsoa-c-f32.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/bsparse_scsoa-c-f32.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/dense_packedacrm_f64: $(OUTDIR)/.make $(BLDDIR)/dense_packedacrm-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/dense_packedacrm-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/dense_packedacrm_f32: $(OUTDIR)/.make $(BLDDIR)/dense_packedacrm-c-f32.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/dense_packedacrm-c-f32.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/dense_packedbcrm_f64: $(OUTDIR)/.make $(BLDDIR)/dense_packedbcrm-c.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/dense_packedbcrm-c.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(OUTDIR)/dense_packedbcrm_f32: $(OUTDIR)/.make $(BLDDIR)/dense_packedbcrm-c-f32.o $(LIBDEP)
	$(LD) -o $@ $(BLDDIR)/dense_packedbcrm-c-f32.o $(MAINLIB) $(SLDFLAGS) $(LDFLAGS) $(CLDFLAGS)

$(BLDDIR)/%-cpp.o: $(SRCDIR)/%.cpp .state $(BLDDIR)/.make $(HEADERS) Makefile $(DEPDIR)/Makefile.inc
	$(CXX) $(DFLAGS) $(IFLAGS) $(CXXFLAGS) $(CTARGET) -c $< -o $@

$(BLDDIR)/%-c.o: $(SRCDIR)/%.c .state $(BLDDIR)/.make $(HEADERS) Makefile $(DEPDIR)/Makefile.inc
	$(CC) $(DFLAGS) $(IFLAGS) $(CFLAGS) $(CTARGET) -c $< -o $@

$(BLDDIR)/%-cpp-f32.o: $(SRCDIR)/%.cpp .state $(BLDDIR)/.make $(HEADERS) Makefile $(DEPDIR)/Makefile.inc
	$(CXX) -D__EDGE_EXECUTE_F32__ $(DFLAGS) $(IFLAGS) $(CXXFLAGS) $(CTARGET) -c $< -o $@

$(BLDDIR)/%-c-f32.o: $(SRCDIR)/%.c .state $(BLDDIR)/.make $(HEADERS) Makefile $(DEPDIR)/Makefile.inc
	$(CC) -D__EDGE_EXECUTE_F32__ $(DFLAGS) $(IFLAGS) $(CFLAGS) $(CTARGET) -c $< -o $@

.PHONY: clean
clean:
ifneq ($(call qapath,$(BLDDIR)),$(ROOTDIR))
ifneq ($(call qapath,$(BLDDIR)),$(call qapath,.))
	@rm -rf $(BLDDIR)
endif
endif
ifneq (,$(wildcard $(BLDDIR))) # still exists
	@rm -f $(OBJECTS) $(OBJECTX) $(FTNOBJS) $(FTNOBJX) *__genmod.* fit.log *.dat
	@rm -f $(BLDDIR)/*.gcno $(BLDDIR)/*.gcda $(BLDDIR)/*.gcov
endif
	@rm -f .make .state

.PHONY: realclean
realclean: clean
ifneq ($(call qapath,$(OUTDIR)),$(ROOTDIR))
ifneq ($(call qapath,$(OUTDIR)),$(call qapath,.))
	@rm -rf $(OUTDIR)
endif
endif
ifneq (,$(wildcard $(OUTDIR))) # still exists
	@rm -f $(OUTDIR)/libxsmm.$(DLIBEXT) $(OUTDIR)/*.stackdump
	@rm -f $(XFILES) $(MODULES)
endif

