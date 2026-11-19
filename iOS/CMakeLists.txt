# Fills the app bundle with the System and script files and packs build/ios-arm64/UT2004.ipa.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(UT_IOS_BUNDLE_ID "org.ut2004.game" CACHE STRING "Bundle identifier, it must match the provisioning profile when signing")
set(UT_IOS_SIGN_IDENTITY "" CACHE STRING "codesign identity, empty leaves the app unsigned for a sideloading tool to sign")
set(UT_IOS_PROVISIONING_PROFILE "" CACHE FILEPATH ".mobileprovision used with UT_IOS_SIGN_IDENTITY")

# An update must carry a higher build number than the installed app, the commit count grows with every change.
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE UT_IOS_BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT UT_IOS_BUILD_NUMBER)
  set(UT_IOS_BUILD_NUMBER 1)
endif()
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
set_target_properties(SDLLaunch PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${UT_IOS_BUNDLE_ID})
target_link_libraries(SDLLaunch PRIVATE OpenAL::OpenAL
  "-framework UIKit" "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics"
  "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMotion"
  "-framework CoreHaptics" "-framework GameController" "-framework Metal" "-framework CoreBluetooth"
  "-framework Security" "-framework SystemConfiguration")

set(UT_APP $<TARGET_BUNDLE_DIR:SDLLaunch>)
set(UT_ICON ${CMAKE_SOURCE_DIR}/Android/app/src/main/res/mipmap-mdpi/ic_launcher.png)
set(UT_IPA_DIR ${CMAKE_BINARY_DIR}/ipa)
if(UT_IOS_SIGN_IDENTITY)
  if(NOT EXISTS "${UT_IOS_PROVISIONING_PROFILE}")
    message(FATAL_ERROR "UT_IOS_SIGN_IDENTITY needs UT_IOS_PROVISIONING_PROFILE")
  endif()
  set(UT_SIGN COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/sign.py --app ${UT_APP}
    --identity ${UT_IOS_SIGN_IDENTITY} --profile ${UT_IOS_PROVISIONING_PROFILE})
endif()
# The packing runs in it, so it stays and only its contents are replaced.
file(MAKE_DIRECTORY ${UT_IPA_DIR})
add_custom_target(ut_ipa
  COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/pack_bundle.py --out ${UT_APP}/Game
          --source ${CMAKE_SOURCE_DIR} --scripts ${UT_SCRIPTS_DIR}
  # Home screen icons for iPhone and iPad, scaled up from the Android one.
  COMMAND sips -z 120 120 ${UT_ICON} --out ${UT_APP}/AppIcon60x60@2x.png
  COMMAND sips -z 180 180 ${UT_ICON} --out ${UT_APP}/AppIcon60x60@3x.png
  COMMAND sips -z 152 152 ${UT_ICON} --out ${UT_APP}/AppIcon76x76@2x~ipad.png
  COMMAND sips -z 167 167 ${UT_ICON} --out ${UT_APP}/AppIcon83.5x83.5@2x~ipad.png
  ${UT_SIGN}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${UT_IPA_DIR}/Payload ${CMAKE_BINARY_DIR}/UT2004.ipa
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UT_IPA_DIR}/Payload/UT2004.app
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${UT_APP} ${UT_IPA_DIR}/Payload/UT2004.app
  # zip keeps the executable bit, which the device needs.
  COMMAND zip -qry ${CMAKE_BINARY_DIR}/UT2004.ipa Payload
  WORKING_DIRECTORY ${UT_IPA_DIR}
  DEPENDS SDLLaunch ut_stage
  USES_TERMINAL VERBATIM)
