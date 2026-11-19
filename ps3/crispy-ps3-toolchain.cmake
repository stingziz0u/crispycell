# CMake toolchain file to cross-compile Crispy Doom for PS3, nativo
# (sin SDL2), usando el toolchain estandar de ps3dev/PSL1GHT -- el mismo
# que TyrQuakeCell.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)
set(PS3DEV /usr/local/ps3dev)  # toolchain estandar, no el fork -aqua

set(CMAKE_C_COMPILER   ${PS3DEV}/ppu/bin/ppu-gcc)
set(CMAKE_CXX_COMPILER ${PS3DEV}/ppu/bin/ppu-g++)
set(CMAKE_FIND_ROOT_PATH ${PS3DEV}/ppu ${PS3DEV}/portlibs/ppu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXE_LINKER_FLAGS "-L${PS3DEV}/ppu/lib -L${PS3DEV}/portlibs/ppu/lib -lnet -lnetctl" CACHE STRING "" FORCE)

include_directories(${PS3DEV}/ppu/include)

# -fno-tree-vectorize: the PPU GCC's automatic AltiVec code (on at -O3)
# miscompiled Hexen's P_LoadVertexes. Into a 16-byte aligned array, vertexes
# 2-3 of every 4 got another vertex's coordinates (vupkhsh/vupklsh, vmrghw,
# stvx); into an array 8 bytes off, the scalar path ran and they were
# right. So whether a new game crashed depended on where the zone put the
# array (polyobj chains that never close: IterFindPolySegs recursing until
# the stack overflowed). Nothing here was written for the vectorizer, and a
# loop it gets wrong goes wrong silently: it stays off for all of it.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cell -fomit-frame-pointer -fno-tree-vectorize")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mcpu=cell -fomit-frame-pointer -fno-tree-vectorize")
