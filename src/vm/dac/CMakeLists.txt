
include(${CLR_DIR}/dac.cmake)

add_precompiled_header(common.h ../common.cpp VM_SOURCES_DAC)
add_library(cee_dac ${VM_SOURCES_DAC})