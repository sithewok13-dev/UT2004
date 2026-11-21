set(CORE_SRC Core.cpp UExporter.cpp UFactory.cpp UnAnsi.cpp UnArc.cpp UnBits.cpp UnCache.cpp
    UnClass.cpp UnCoreNative.cpp UnCoreNet.cpp UnCorSc.cpp UnLinker.cpp UnMath.cpp
    UnMem.cpp UnMisc.cpp UnName.cpp UnObj.cpp UnProp.cpp)
if(WIN32)
  ut_module(Core SHARED
    SOURCES ${CORE_SRC} UnVcWin32.cpp
    PCH CorePrivate.h Core.cpp
    NOPCH UnAnsi.cpp UnVcWin32.cpp
    DEPS Vorbis::vorbisfile winmm shlwapi)
else()
  ut_module(Core SHARED SOURCES ${CORE_SRC} UnUnix.cpp DEPS Vorbis::vorbisfile)
  if(IOS)
    # Waits that keep the run loop going, so touches arrive on time (UnIOS.cpp).
    target_sources(Core PRIVATE Src/UnIOS.cpp)
    target_link_libraries(Core PUBLIC "-framework CoreFoundation")
  endif()
endif()

# Read at configure time, so the log of a build names the commit it was configured at.
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE UT_PORT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set_property(SOURCE Src/UnMisc.cpp APPEND PROPERTY COMPILE_DEFINITIONS UT_PORT_VERSION="${UT_PORT_VERSION}")
