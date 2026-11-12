cmake_minimum_required(VERSION 3.25)
project(WebrtcVoiceApo LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build x64 only (audiodg.exe is 64-bit).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: audiodg must not depend on a VC++ redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Upstream WebRTC (AEC3, NS, AGC2, limiter), built from pinned sources (scripts/Fetch-Upstream.ps1)
include(cmake/WebrtcUpstream.cmake)

# --- Settings + log (shared by the audio effect and the settings app; no WebRTC)
add_library(settings_core STATIC src/Settings.cpp src/Settings.h src/Log.cpp src/Log.h src/Telemetry.cpp src/Telemetry.h)
target_compile_definitions(settings_core PUBLIC UNICODE _UNICODE NOMINMAX)
target_compile_options(settings_core PRIVATE /W4 /guard:cf)
target_link_libraries(settings_core PUBLIC advapi32 ole32)

# --- Our processing core
add_library(voice_core STATIC
  third_party/speexdsp/resample.c
  src/VoiceProcessor.cpp
  src/VoiceProcessor.h
  src/AgcStage.cpp
  src/AgcStage.h
  src/AecConfig.h
  src/ReferenceRing.h
)
target_include_directories(voice_core PUBLIC third_party/speexdsp)
set_source_files_properties(third_party/speexdsp/resample.c PROPERTIES COMPILE_OPTIONS "/W0")
target_compile_definitions(voice_core PUBLIC NOMINMAX _USE_MATH_DEFINES)
target_compile_options(voice_core PRIVATE /W3 /guard:cf)
target_link_libraries(voice_core PUBLIC webrtc_apm settings_core)

# --- Settings app (a normal desktop app: it DOES get a manifest, unlike the APO DLL)
add_executable(WebrtcAudioEngineSettings WIN32 app/SettingsApp.cpp app/TrayFlyout.cpp app/App.h app/SettingsApp.manifest)
target_link_libraries(WebrtcAudioEngineSettings PRIVATE settings_core comctl32 shell32 user32 gdi32 dwmapi)
target_compile_options(WebrtcAudioEngineSettings PRIVATE /W4 /permissive- /guard:cf)
target_link_options(WebrtcAudioEngineSettings PRIVATE /guard:cf)

add_library(WebrtcVoiceApo SHARED
  src/DllMain.cpp
  src/ProbeApo.cpp
  src/ProbeApo.h
  src/Guids.h
  src/WebrtcVoiceApo.def
)
target_link_libraries(WebrtcVoiceApo PRIVATE voice_core)

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

# Offline tests of the processing core.
add_executable(ProcessorTests tests/ProcessorTests.cpp)
target_link_libraries(ProcessorTests PRIVATE voice_core)

# Opens the mic through WASAPI in a chosen category / RAW mode and reports errors + level.
add_executable(CaptureTest tests/CaptureTest.cpp)
target_compile_definitions(CaptureTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CaptureTest PRIVATE ole32 uuid settings_core)

# Plays octave tones and compares the played level with the WASAPI loopback and our echo reference
# (does speaker processing such as Dolby sit before the reference tap?).
add_executable(PathProbe tests/PathProbe.cpp)
target_compile_definitions(PathProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PathProbe PRIVATE ole32 uuid settings_core)
