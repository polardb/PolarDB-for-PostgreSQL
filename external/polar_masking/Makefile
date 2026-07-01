MODULE_big = polar_masking
OBJS = \
	$(WIN32RES) \
	polar_masking.o \
	polar_masking_label.o

EXTENSION = polar_masking
DATA = polar_masking--1.0.0.sql
PGFILEDESC = "data masking for polardb"

REGRESS_OPTS = --temp-config polar_masking.conf
REGRESS = polar_masking
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = external/polar_masking
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
