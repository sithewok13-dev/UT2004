if(WIN32 OR IOS OR EMSCRIPTEN)
  set(SDLLAUNCH_OUTPUT UT2004)
else()
  set(SDLLAUNCH_OUTPUT ut2004-bin)
endif()
if(NOT EMSCRIPTEN)
  set(SDLLAUNCH_CONTENT SDLLaunchContent.cpp HttpFile.cpp)
endif()
ut_module(SDLLaunch WIN32 OUTPUT ${SDLLAUNCH_OUTPUT} SOURCES SDLLaunch.cpp ${SDLLAUNCH_CONTENT} DEPS Engine SDL3::SDL3 ZLIB::ZLIB)
if(UT_STATIC_LINK)
  target_link_libraries(SDLLaunch PRIVATE SDLDrv OpenGLDrv ALAudio Fire IpDrv UWeb XGame XInterface Onslaught Editor ${GLES_TARGET})
  # UnNullRenderDevice.h includes EnginePrivate.h.
  target_include_directories(SDLLaunch PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Src ${CMAKE_SOURCE_DIR}/OpenGLDrv/Src)
endif()
if(WIN32)
  target_sources(SDLLaunch PRIVATE Src/SDLLaunch.rc)
  # A GUI subsystem exe that still enters through main.
  target_link_options(SDLLaunch PRIVATE /ENTRY:mainCRTStartup)
endif()
if(ANDROID)
  target_sources(SDLLaunch PRIVATE Src/SDLLaunchAndroid.cpp Src/SDLLaunchTar.cpp)
  # System and script files ship xz compressed inside the APK, the content comes as a .tar.xz on first run.
  target_link_libraries(SDLLaunch PRIVATE LibLZMA::LibLZMA)
endif()
if(IOS)
  # The content comes as a .tar.xz on first run, or as an archive the player puts in the app's folder.
  target_sources(SDLLaunch PRIVATE Src/SDLLaunchIOS.cpp Src/SDLLaunchTar.cpp)
  target_link_libraries(SDLLaunch PRIVATE LibLZMA::LibLZMA OpenSSL::SSL)
  target_include_directories(SDLLaunch PRIVATE ${GLES3_INCLUDE_DIR})
endif()
if(NOT EMSCRIPTEN)
  # Both are C libraries, so unlike IpDrv's C++ ones they are safe in the executable before the allocator is set.
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  target_link_libraries(SDLLaunch PRIVATE OpenSSL::Crypto CURL::libcurl)
  # The content downloads. Read on every configure unless set with -D, so test builds can point at localhost.
  set(UT_ASSETS_ENV ${CMAKE_SOURCE_DIR}/Android/assets.env)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UT_ASSETS_ENV})
  # The digest and sizes belong to the hosted file, so a URL set with -D only gets what is set with -D too.
  macro(ut_content_source name)
    if(NOT UT_${name}_URL)
      foreach(key URL ${ARGN})
        file(STRINGS ${UT_ASSETS_ENV} UT_${name}_${key} REGEX "^UT_${name}_${key}=")
        string(REGEX REPLACE "^UT_${name}_${key}=" "" UT_${name}_${key} "${UT_${name}_${key}}")
      endforeach()
    endif()
  endmacro()
  ut_content_source(ASSETS SHA256)
  ut_content_source(FULL SHA256 SIZE UNPACKED)
  foreach(key SIZE UNPACKED)
    if(NOT UT_FULL_${key})
      set(UT_FULL_${key} 0)
    endif()
  endforeach()
  target_compile_definitions(SDLLaunch PRIVATE UT_ASSETS_URL="${UT_ASSETS_URL}" UT_ASSETS_SHA256="${UT_ASSETS_SHA256}"
    UT_FULL_URL="${UT_FULL_URL}" UT_FULL_SHA256="${UT_FULL_SHA256}" UT_FULL_SIZE=${UT_FULL_SIZE}LL UT_FULL_UNPACKED=${UT_FULL_UNPACKED}LL)
endif()
if(EMSCRIPTEN OR IOS)
  # appGetDllHandle can't dlsym in a browser, and on iOS the linker would drop the unreferenced symbols,
  # so it looks the linked packages up in this table.
  get_property(pkgs GLOBAL PROPERTY UT_LINKED_PACKAGES)
  set(decls "")
  set(checks "")
  foreach(pkg ${pkgs})
    string(APPEND decls "extern unsigned char GLoaded${pkg};\n")
    string(APPEND checks "\tif (!strcmp(Symbol, \"GLoaded${pkg}\")) return &GLoaded${pkg};\n")
  endforeach()
  file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LinkedPackages.cpp CONTENT
    "#include <string.h>\n${decls}extern \"C\" void* appLinkedPackage(const char* Symbol)\n{\n${checks}\treturn 0;\n}\n")
  target_sources(SDLLaunch PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/LinkedPackages.cpp)
endif()
if(EMSCRIPTEN)
  target_sources(SDLLaunch PRIVATE Src/SDLLaunchWeb.cpp)

  set(webrtc ${CMAKE_SOURCE_DIR}/Emscripten/webrtc.js)
  set(content ${CMAKE_SOURCE_DIR}/Emscripten/content.js)
  # Emscripten/index.html loads UT2004.js after the system package loader.
  set_target_properties(SDLLaunch PROPERTIES SUFFIX .js RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR} LINK_DEPENDS "${webrtc};${content}")
  target_link_options(SDLLaunch PRIVATE
    # Garbage collection and script recursion need far more than the 64 KB default.
    -sSTACK_SIZE=8MB
    -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=512MB -sMAXIMUM_MEMORY=4GB
    # Without FULL_ES3 only what WebGL 2 offers natively links.
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS
    -lopenal -lidbfs.js -sFETCH -sFORCE_FILESYSTEM -sEXIT_RUNTIME=0 -sENVIRONMENT=web
    -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,addRunDependency,removeRunDependency
    # Lets the engine wait for content from IndexedDB and for the next animation frame.
    -sJSPI
    "SHELL:--js-library ${webrtc}" "SHELL:--js-library ${content}")
endif()
