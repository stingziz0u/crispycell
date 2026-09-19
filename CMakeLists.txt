set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

cmake_minimum_required(VERSION 3.7.2)
project("Crispy Doom" VERSION 7.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

# Autotools variables
set(top_srcdir ${CMAKE_CURRENT_SOURCE_DIR})
set(top_builddir ${CMAKE_CURRENT_BINARY_DIR})

# AC_INIT variables
set(PACKAGE_NAME "${PROJECT_NAME}")
set(PACKAGE_TARNAME "crispy-doom")
set(PACKAGE_VERSION "${PROJECT_VERSION}")
set(PACKAGE_STRING "${PROJECT_NAME} ${PROJECT_VERSION}")
set(PACKAGE_BUGREPORT "https://github.com/fabiangreffrath/crispy-doom/issues")

string(REGEX REPLACE " Doom$" "" PACKAGE_SHORTNAME "${PACKAGE_NAME}")
set(PACKAGE_COPYRIGHT "Copyright (C) 1993-2025")
set(PACKAGE_LICENSE "GNU General Public License, version 2")

# Any settings that should apply to all targets in this directory and all
# subdirectories should go here.  Use judiciously.
if(MSVC)
    add_definitions("/D_CRT_SECURE_NO_WARNINGS" "/D_CRT_SECURE_NO_DEPRECATE"
                    "/D_CRT_NONSTDC_NO_DEPRECATE")
else()
    add_compile_options("-Wall" "-Wdeclaration-after-statement"
                        "-Wredundant-decls")
endif()

option(CMAKE_FIND_PACKAGE_PREFER_CONFIG
       "Lookup package config files before using find modules" On)

option(ENABLE_SDL2_NET "Enable SDL2_net" On)
option(ENABLE_SDL2_MIXER "Enable SDL2_mixer" On)
option(PS3_BUILD "Cross-compile for PS3 (PSL1GHT), no SDL2" OFF)

if(PS3_BUILD)
    set(ENABLE_SDL2_NET OFF CACHE BOOL "" FORCE)
    set(ENABLE_SDL2_MIXER OFF CACHE BOOL "" FORCE)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    add_library(SDL2::SDL2main INTERFACE IMPORTED)
    add_compile_definitions(PS3_BUILD=1)

    # Read the package identity from ps3/appid.mk, the same file
    # Makefile.crispypkg includes, so the paths compiled into the binary
    # always match the APPID the .pkg installs under.
    # Re-run cmake when appid.mk changes. file(STRINGS) reads it but
    # does not register it as a dependency, so without this a switch
    # from one game to another leaves every already-built object with
    # the previous PS3_GAME_* define -- the launcher would still say
    # CRISPY HEXEN in a Strife build.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PROJECT_SOURCE_DIR}/ps3/appid.mk")

    file(STRINGS "${PROJECT_SOURCE_DIR}/ps3/appid.mk" _ps3_appid_lines)
    foreach(_line IN LISTS _ps3_appid_lines)
        if(_line MATCHES "^PS3_APPID[ \t]*:=[ \t]*(.+)$")
            string(STRIP "${CMAKE_MATCH_1}" PS3_APPID)
        endif()
    endforeach()
    if(NOT PS3_APPID)
        message(FATAL_ERROR "PS3_APPID not found in ps3/appid.mk")
    endif()
    string(LENGTH "${PS3_APPID}" _appid_len)
    if(NOT _appid_len EQUAL 9)
        message(FATAL_ERROR "PS3_APPID '${PS3_APPID}' is ${_appid_len} chars, must be exactly 9")
    endif()
    foreach(_line IN LISTS _ps3_appid_lines)
        if(_line MATCHES "^PS3_GAME[ \t]*:=[ \t]*(.+)$")
            string(STRIP "${CMAKE_MATCH_1}" PS3_GAME)
        endif()
    endforeach()
    if(NOT PS3_GAME)
        set(PS3_GAME "doom")
    endif()
    string(TOUPPER "${PS3_GAME}" _ps3_game_upper)
    add_compile_definitions(PS3_GAME_${_ps3_game_upper}=1)
    message(STATUS "PS3 APPID: ${PS3_APPID}  GAME: ${PS3_GAME}")
    add_compile_definitions(PS3_USRDIR="/dev_hdd0/game/${PS3_APPID}/USRDIR")
endif()

if (NOT DEFINED EMSCRIPTEN AND NOT PS3_BUILD)
    find_package(SDL2 2.0.14)
endif()
if(ENABLE_SDL2_MIXER)
    if (NOT DEFINED EMSCRIPTEN)
        find_package(SDL2_mixer 2.0.2)
    endif()
else()
    add_compile_definitions(DISABLE_SDL2MIXER=1)
endif()

if(ENABLE_SDL2_NET)
    if (NOT DEFINED EMSCRIPTEN)
        find_package(SDL2_net 2.0.0)
    endif()
else()
    add_compile_definitions(DISABLE_SDL2NET=1)
endif()

option(CRISPY_TRUECOLOR "True color rendering" OFF)

# Check for libsamplerate.
find_package(SampleRate)
if(SampleRate_FOUND)
    set(HAVE_LIBSAMPLERATE TRUE)
endif()

# Check for libpng.
find_package(PNG)
if(PNG_FOUND)
    set(HAVE_LIBPNG TRUE)
endif()

# Check for FluidSynth.
find_package(FluidSynth 2.2.0)
if(FluidSynth_FOUND AND ENABLE_SDL2_MIXER)
    set(HAVE_FLUIDSYNTH TRUE)
endif()

find_package(m)

include(CheckSymbolExists)
include(CheckIncludeFile)
check_symbol_exists(strcasecmp "strings.h" HAVE_DECL_STRCASECMP)
check_symbol_exists(strncasecmp "strings.h" HAVE_DECL_STRNCASECMP)
check_include_file("dirent.h" HAVE_DIRENT_H)

string(CONCAT WINDOWS_RC_VERSION "${PROJECT_VERSION_MAJOR}, "
    "${PROJECT_VERSION_MINOR}, ${PROJECT_VERSION_PATCH}, 0")

# Without a hyphen. This is used for the bash-completion scripts.
string(TOLOWER "${PACKAGE_SHORTNAME}" PROGRAM_SPREFIX)

# With a hyphen, used almost everywhere else.
set(PROGRAM_PREFIX "${PROGRAM_SPREFIX}-")

configure_file(cmake/config.h.cin config.h)

configure_file(src/doom-res.rc.in src/doom-res.rc)
configure_file(src/heretic-res.rc.in src/heretic-res.rc)
configure_file(src/hexen-res.rc.in src/hexen-res.rc)
configure_file(src/setup-res.rc.in src/setup-res.rc)
configure_file(src/setup/setup-manifest.xml.in src/setup/setup-manifest.xml)
configure_file(src/strife-res.rc.in src/strife-res.rc)

# textscreen is the SDL-based text UI (the setup program, and Heretic's
# and Hexen's startup screens). Nothing in the PS3 build links it -- it
# is already out of EXTRA_LIBS -- but compiling the subdirectory still
# needs SDL headers that do not exist here.
if(PS3_BUILD)
    set(CRISPY_SUBDIRS opl pcsound src)
else()
    set(CRISPY_SUBDIRS textscreen opl pcsound src)
endif()

foreach(SUBDIR ${CRISPY_SUBDIRS})
    add_subdirectory("${SUBDIR}")
endforeach()
