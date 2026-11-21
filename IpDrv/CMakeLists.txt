if(EMSCRIPTEN)
  # Browsers have no UDP, they join and host through their own WebRTC.
  set(IPDRV_WEBRTC WebRtcBrowser.cpp)
else()
  set(IPDRV_WEBRTC WebRtc.cpp)
endif()
ut_module(IpDrv SHARED
  SOURCES HTTPDownload.cpp InternetLink.cpp IpDrv.cpp MasterServerClient.cpp
          MasterServerUplink.cpp TcpLink.cpp TcpNetDriver.cpp UCompressCommandlet.cpp
          UdpLink.cpp UnIpDrvNative.cpp UnSocket.cpp WebMaster.cpp WebRtcCommon.cpp ${IPDRV_WEBRTC}
  PCH UnIpDrv.h IpDrv.cpp
  NOPCH WebMaster.cpp WebRtcCommon.cpp ${IPDRV_WEBRTC}
  DEPS Engine $<$<PLATFORM_ID:Windows>:wsock32>)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(IpDrv PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(IpDrv PROPERTIES CXX_STANDARD 17)
if(NOT EMSCRIPTEN)
  find_package(OpenSSL REQUIRED)
  find_package(CURL REQUIRED)
  if(WIN32 OR ANDROID OR IOS)
    find_package(LibDataChannel CONFIG REQUIRED)
    set(IPDRV_DATACHANNEL LibDataChannel::LibDataChannel)
  else()
    # No distribution packages libdatachannel, so the release vcpkg uses is built static with the dependencies its submodules pin.
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(NO_MEDIA ON)
    set(NO_WEBSOCKET ON)
    set(NO_EXAMPLES ON)
    set(NO_TESTS ON)
    # The same patch as the vcpkg port. A changed patch gets a fresh source dir, since patching an already patched tree fails.
    set(DATACHANNEL_PATCH ${CMAKE_SOURCE_DIR}/ports/libdatachannel/stopped-flag.patch)
    file(SHA256 ${DATACHANNEL_PATCH} DATACHANNEL_HASH)
    string(SUBSTRING ${DATACHANNEL_HASH} 0 12 DATACHANNEL_HASH)
    FetchContent_Declare(libdatachannel
      SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/libdatachannel-src-${DATACHANNEL_HASH}
      GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
      # v0.24.6, by hash so a moved tag cannot change what gets built
      GIT_TAG 6b1e2e620f1e37f0eafeee702eaea0043cb305fd
      # A git source updates on every configure, which would patch the tree again and fail
      UPDATE_DISCONNECTED ON
      PATCH_COMMAND patch -p1 -i ${DATACHANNEL_PATCH})
    FetchContent_MakeAvailable(libdatachannel)
    set(IPDRV_DATACHANNEL datachannel)
  endif()
  target_link_libraries(IpDrv PRIVATE ${IPDRV_DATACHANNEL} OpenSSL::Crypto CURL::libcurl)
  if(IOS)
    # OpenSSL has no CA store on iOS, WebMaster.cpp hands each server chain to the system's trust evaluation.
    target_sources(IpDrv PRIVATE Src/AppleTrust.cpp)
    target_link_libraries(IpDrv PRIVATE OpenSSL::SSL "-framework Security" "-framework CoreFoundation")
  endif()
endif()
