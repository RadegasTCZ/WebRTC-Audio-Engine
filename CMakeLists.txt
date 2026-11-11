cmake_minimum_required(VERSION 3.25)
project(WebrtcVoiceApo LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build x64 only (audiodg.exe is 64-bit).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: audiodg must not depend on a VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(WebrtcVoiceApo SHARED
  src/DllMain.cpp
  src/ProbeApo.cpp
  src/ProbeApo.h
  src/Log.cpp
  src/Log.h
  src/Guids.h
  src/WebrtcVoiceApo.def
)

target_compile_definitions(WebrtcVoiceApo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(WebrtcVoiceApo PRIVATE /W4 /permissive- /EHsc /guard:cf)

# No embedded manifest: an APO DLL with a manifest fails to load in protected audiodg.
target_link_options(WebrtcVoiceApo PRIVATE /MANIFEST:NO /guard:cf /DYNAMICBASE /NXCOMPAT)

target_link_libraries(WebrtcVoiceApo PRIVATE
  audiobaseprocessingobject  # CBaseAudioProcessingObject (Windows SDK)
  audiomediatypecrt          # CreateAudioMediaType helpers used by the base class
  ole32 oleaut32 uuid advapi32 propsys
)

# Offline smoke test that drives the APO like audiodg does (no install needed).
add_executable(ProbeHarness tests/ProbeHarness.cpp)
target_compile_definitions(ProbeHarness PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ProbeHarness PRIVATE audiomediatypecrt ole32 uuid)
add_dependencies(ProbeHarness WebrtcVoiceApo)
