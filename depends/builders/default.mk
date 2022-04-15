default_build_CC = gcc
default_build_CXX = g++
default_build_AR = ar
default_build_TAR = tar
default_build_RANLIB = ranlib
default_build_STRIP = strip
default_build_NM = nm

define add_build_tool_func
build_$(build_os)_$1 ?= $$(default_build_$1)
build_$(build_arch)_$(build_os)_$1 ?= $$(build_$(build_os)_$1)
build_$1=$$(build_$(build_arch)_$(build_os)_$1)
endef
$(foreach var,CC CXX AR TAR RANLIB NM STRIP SHA256SUM DOWNLOAD OTOOL INSTALL_NAME_TOOL,$(eval $(call add_build_tool_func,$(var))))
define add_build_flags_func
build_$(build_arch)_$(build_os)_$1 += $(build_$(build_os)_$1)
build_$1=$$(build_$(build_arch)_$(build_os)_$1)
endef
$(foreach flags, CFLAGS CXXFLAGS LDFLAGS, $(eval $(call add_build_flags_func,$(flags))))
