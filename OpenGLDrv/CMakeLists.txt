ut_module(OpenGLDrv SHARED
  SOURCES OpenGLCompiler.cpp OpenGLDrv.cpp OpenGLFog.cpp OpenGLMaterial.cpp OpenGLRenderDevice.cpp OpenGLPostProcess.cpp OpenGLRenderInterface.cpp OpenGLResource.cpp
  PCH OpenGLDrv.h OpenGLDrv.cpp
  DEPS Engine)
target_link_libraries(OpenGLDrv PRIVATE SDL3::SDL3 ${GLES_TARGET} ${SQUISH_TARGET})
if(IOS)
  target_include_directories(OpenGLDrv PRIVATE ${GLES3_INCLUDE_DIR})
endif()
if(WIN32)
  # CMake copies no runtime DLLs on its own, and SDL loads libEGL by name.
  add_custom_command(TARGET OpenGLDrv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE:${GLES_TARGET}> $<TARGET_FILE:unofficial::angle::libEGL> ${UT_SYSTEM_DIR})
endif()
