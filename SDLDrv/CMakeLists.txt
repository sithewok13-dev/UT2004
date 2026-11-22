ut_module(SDLDrv SHARED SOURCES SDLClient.cpp SDLDrv.cpp SDLViewport.cpp DEPS Engine SDL3::SDL3)
# GamepadMotion.hpp uses std::clamp.
set_target_properties(SDLDrv PROPERTIES CXX_STANDARD 17)
if(IOS)
  # The touch overlay (the owner's DOOM port's, iOS/touch/IOSTouch.m) and UT2004's side of it.
  # Objective-C with ARC, kept apart from the engine's compile flags.
  enable_language(OBJC)
  add_library(iostouch STATIC ${CMAKE_SOURCE_DIR}/iOS/touch/IOSTouch.m ${CMAKE_SOURCE_DIR}/iOS/touch/IOSAudioSession.m)
  target_include_directories(iostouch PUBLIC ${CMAKE_SOURCE_DIR}/iOS/touch)
  target_compile_options(iostouch PRIVATE -fobjc-arc)
  target_link_libraries(iostouch PRIVATE SDL3::SDL3
    PUBLIC "-framework UIKit" "-framework QuartzCore" "-framework CoreMotion" "-framework AudioToolbox" "-framework AVFoundation")
  target_sources(SDLDrv PRIVATE Src/SDLTouchIOS.cpp)
  target_link_libraries(SDLDrv PRIVATE iostouch)
endif()
