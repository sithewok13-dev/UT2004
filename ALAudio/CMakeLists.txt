# OpenAL itself is loaded at runtime (ALFuncs.h), only its headers are needed.
ut_module(ALAudio SHARED
  SOURCES ALAudio.cpp ALAudioCapture.cpp ALAudioStream.cpp ALAudioSubsystem.cpp ALAudioVoice.cpp
  PCH ALAudioPrivate.h ALAudio.cpp
  DEPS Engine speex)
if(WIN32)
  target_include_directories(ALAudio PRIVATE $<TARGET_PROPERTY:OpenAL::OpenAL,INTERFACE_INCLUDE_DIRECTORIES>)
  # Nothing links OpenAL, so it is not copied next to the executables on its own.
  add_custom_command(TARGET ALAudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenAL::OpenAL> ${UT_SYSTEM_DIR})
elseif(IOS)
  # Linked in, so ALAudio binds the functions directly, see ALAudioSubsystem.h.
  target_link_libraries(ALAudio PRIVATE OpenAL::OpenAL)
else()
  target_include_directories(ALAudio PRIVATE ${OPENAL_INCLUDE_DIR})
endif()
