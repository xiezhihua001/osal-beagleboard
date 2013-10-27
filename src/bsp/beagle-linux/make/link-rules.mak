###############################################################################
# File: link-rules.mak
#
# Purpose:
#   Makefile for linking code and producing an executable image.
#
# History:
#
###############################################################################


##
## Linker flags that are needed
##
LDFLAGS = --sysroot=/home/export/angstrom/setup-scripts/build/tmp-angstrom_v2013_12-eglibc/sysroots/beaglebone

##
## Libraries to link in
## Cygwin cannot use the -lrt switch
## Older versions of linux may not have this as well.
##
OSTYPE := $(shell uname -s)
ifeq ($(OSTYPE),Linux)
  LIBS = -lpthread -lrt -ldl
else
  LIBS = -lpthread  -ldl
endif


##
## OSAL Core Link Rule
## 
$(APPTARGET).$(APP_EXT): $(OBJS)
	$(COMPILER) $(DEBUG_FLAGS) -o $(APPTARGET).$(APP_EXT) $(OBJS) $(CORE_OBJS) $(LDFLAGS) $(LIBS)
	

