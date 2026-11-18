# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RadegasTCZ
# Builds the parts of upstream WebRTC we use (AEC3, NS, AGC2 + RNN VAD, limiter, AudioBuffer and their
# dependencies) as one static library `webrtc_apm` from the pinned sources that scripts\Fetch-Upstream.ps1
# downloads into <workspace>\upstream. File lists follow the companion AEC3-Echo-Canceller plugin recipe
# (tools/aec3-build/CMakeLists.txt, same revisions); see third_party/UPSTREAM.md.

set(UPSTREAM_DIR "${CMAKE_SOURCE_DIR}/../upstream" CACHE PATH "Pinned upstream sources (scripts/Fetch-Upstream.ps1)")
set(W "${UPSTREAM_DIR}/webrtc")
set(CHROMIUM_TP "${UPSTREAM_DIR}/chromium")
set(ABSEIL_DIR "${UPSTREAM_DIR}/abseil-cpp")
set(STUBS_DIR "${CMAKE_SOURCE_DIR}/third_party/webrtc_stubs")
foreach(f "${W}/modules/audio_processing/aec3/echo_canceller3.cc" "${ABSEIL_DIR}/absl/base/config.h"
          "${CHROMIUM_TP}/third_party/rnnoise/src/rnn_vad_weights.cc")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "Upstream sources missing (${f}). Run scripts\\Fetch-Upstream.ps1 first.")
    endif()
endforeach()

# --- Abseil ---
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(BUILD_TESTING OFF)
set(ABSL_MSVC_STATIC_RUNTIME ON)   # we link the static CRT
add_subdirectory("${ABSEIL_DIR}" "${CMAKE_BINARY_DIR}/abseil-cpp" EXCLUDE_FROM_ALL)

set(AEC3_NAMES adaptive_fir_filter adaptive_fir_filter_erl aec3_common aec3_fft aec_state alignment_mixer
    api_call_jitter_metrics block_buffer block_delay_buffer block_framer block_processor block_processor_metrics
    clockdrift_detector coarse_filter_update_gain comfort_noise_generator config_selector decimator
    dominant_nearend_detector downsampled_render_buffer echo_audibility echo_canceller3 echo_path_delay_estimator
    echo_path_variability echo_remover echo_remover_metrics erl_estimator erle_estimator fft_buffer filter_analyzer
    frame_blocker fullband_erle_estimator matched_filter matched_filter_lag_aggregator moving_average_spectrum
    multi_channel_content_detector refined_filter_update_gain render_buffer render_delay_buffer
    render_delay_controller render_delay_controller_metrics render_signal_analyzer residual_echo_estimator
    reverb_decay_estimator reverb_frequency_response reverb_model reverb_model_estimator
    signal_dependent_erle_estimator spectrum_buffer stationarity_estimator subband_erle_estimator
    subband_nearend_detector subtractor subtractor_output subtractor_output_analyzer suppression_filter
    suppression_gain transparent_mode)
list(TRANSFORM AEC3_NAMES PREPEND "${W}/modules/audio_processing/aec3/" OUTPUT_VARIABLE AEC3_SRC)
list(TRANSFORM AEC3_SRC APPEND ".cc")

set(AVX2_SRC
    ${W}/modules/audio_processing/aec3/adaptive_fir_filter_avx2.cc
    ${W}/modules/audio_processing/aec3/adaptive_fir_filter_erl_avx2.cc
    ${W}/modules/audio_processing/aec3/fft_data_avx2.cc
    ${W}/modules/audio_processing/aec3/vector_math_avx2.cc
    ${W}/common_audio/fir_filter_avx2.cc
    ${W}/common_audio/resampler/sinc_resampler_avx2.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/vector_math_avx2.cc)

set(NS_NAMES fast_math histograms noise_estimator noise_suppressor ns_fft prior_signal_model
    prior_signal_model_estimator quantile_noise_estimator signal_model signal_model_estimator
    speech_probability_estimator suppression_params wiener_filter)
list(TRANSFORM NS_NAMES PREPEND "${W}/modules/audio_processing/ns/" OUTPUT_VARIABLE NS_SRC)
list(TRANSFORM NS_SRC APPEND ".cc")

set(AGC2_SRC
    ${W}/modules/audio_processing/agc2/biquad_filter.cc
    ${W}/modules/audio_processing/agc2/adaptive_digital_gain_controller.cc
    ${W}/modules/audio_processing/agc2/cpu_features.cc
    ${W}/modules/audio_processing/agc2/fixed_digital_level_estimator.cc
    ${W}/modules/audio_processing/agc2/gain_applier.cc
    ${W}/modules/audio_processing/agc2/interpolated_gain_curve.cc
    ${W}/modules/audio_processing/agc2/limiter.cc
    ${W}/modules/audio_processing/agc2/limiter_db_gain_curve.cc
    ${W}/modules/audio_processing/agc2/noise_level_estimator.cc
    ${W}/modules/audio_processing/agc2/saturation_protector.cc
    ${W}/modules/audio_processing/agc2/saturation_protector_buffer.cc
    ${W}/modules/audio_processing/agc2/speech_level_estimator.cc
    ${W}/modules/audio_processing/agc2/speech_level_estimator_experimental_impl.cc
    ${W}/modules/audio_processing/agc2/speech_level_estimator_impl.cc
    ${W}/modules/audio_processing/agc2/vad_wrapper.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/auto_correlation.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/features_extraction.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/lp_residual.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/pitch_search.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/pitch_search_internal.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/rnn.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/rnn_fc.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/rnn_gru.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/spectral_features.cc
    ${W}/modules/audio_processing/agc2/rnn_vad/spectral_features_internal.cc
    ${W}/modules/audio_processing/utility/pffft_wrapper.cc
    ${CHROMIUM_TP}/third_party/rnnoise/src/rnn_vad_weights.cc
    ${CHROMIUM_TP}/third_party/pffft/src/pffft.c)

set(MISC_SRC
    ${W}/api/audio/echo_canceller3_config.cc
    ${W}/api/audio/echo_canceller3_factory.cc
    ${W}/api/audio/audio_frame.cc
    ${W}/api/field_trials_registry.cc
    ${W}/api/task_queue/task_queue_base.cc
    ${W}/api/environment/environment_factory.cc
    ${W}/api/environment/deprecated_global_field_trials.cc
    ${W}/api/environment/force_test_environment.cc
    ${W}/modules/audio_processing/audio_buffer.cc
    ${W}/modules/audio_processing/splitting_filter.cc
    ${W}/modules/audio_processing/three_band_filter_bank.cc
    ${W}/modules/audio_processing/logging/apm_data_dumper.cc
    ${W}/modules/audio_processing/high_pass_filter.cc
    ${W}/modules/audio_processing/utility/cascaded_biquad_filter.cc
    ${W}/modules/audio_processing/capture_mixer/capture_mixer.cc
    ${W}/modules/audio_processing/capture_mixer/audio_content_analyzer.cc
    ${W}/modules/audio_processing/capture_mixer/channel_content_remixer.cc
    ${W}/modules/audio_processing/capture_mixer/dc_levels_estimator.cc
    ${W}/modules/audio_processing/capture_mixer/energy_estimator.cc
    ${W}/modules/audio_processing/capture_mixer/remixing_logic.cc
    ${W}/modules/audio_processing/capture_mixer/saturation_estimator.cc
    ${W}/rtc_base/checks.cc
    ${W}/rtc_base/logging.cc
    ${W}/rtc_base/race_checker.cc
    ${W}/rtc_base/ref_counter.cc
    ${W}/rtc_base/random.cc
    ${W}/rtc_base/cpu_info.cc
    ${W}/rtc_base/platform_thread_types.cc
    ${W}/rtc_base/strings/string_builder.cc
    ${W}/rtc_base/experiments/field_trial_parser.cc
    ${W}/rtc_base/memory/aligned_malloc.cc
    ${W}/rtc_base/time_utils.cc
    ${W}/rtc_base/string_encode.cc
    ${W}/rtc_base/string_utils.cc
    ${W}/rtc_base/system_time.cc
    ${W}/system_wrappers/source/clock.cc
    ${W}/system_wrappers/source/metrics.cc)

set(COMMON_AUDIO_SRC
    ${W}/common_audio/audio_converter.cc ${W}/common_audio/audio_util.cc ${W}/common_audio/channel_buffer.cc
    ${W}/common_audio/fir_filter_c.cc ${W}/common_audio/fir_filter_factory.cc ${W}/common_audio/fir_filter_sse.cc
    ${W}/common_audio/real_fourier.cc ${W}/common_audio/real_fourier_ooura.cc ${W}/common_audio/smoothing_filter.cc
    ${W}/common_audio/window_generator.cc ${W}/common_audio/ring_buffer.c
    ${W}/common_audio/resampler/push_resampler.cc ${W}/common_audio/resampler/push_sinc_resampler.cc
    ${W}/common_audio/resampler/resampler.cc ${W}/common_audio/resampler/sinc_resampler.cc
    ${W}/common_audio/resampler/sinc_resampler_sse.cc
    ${W}/common_audio/third_party/ooura/fft_size_128/ooura_fft.cc
    ${W}/common_audio/third_party/ooura/fft_size_128/ooura_fft_sse2.cc
    ${W}/common_audio/third_party/ooura/fft_size_256/fft4g.cc
    ${W}/common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.c
    ${W}/common_audio/vad/vad.cc ${W}/common_audio/vad/vad_core.c ${W}/common_audio/vad/vad_filterbank.c
    ${W}/common_audio/vad/vad_gmm.c ${W}/common_audio/vad/vad_sp.c ${W}/common_audio/vad/webrtc_vad.c)
foreach(n auto_corr_to_refl_coef auto_correlation copy_set_operations cross_correlation division_operations
          downsample_fast energy filter_ar filter_ar_fast_q12 filter_ma_fast_q12 get_hanning_window
          get_scaling_square levinson_durbin lpc_to_refl_coef min_max_operations randomization_functions
          refl_coef_to_lpc resample resample_48khz resample_by_2 resample_by_2_internal resample_fractional
          spl_init spl_inl spl_sqrt splitting_filter vector_operations vector_scaling_operations)
    list(APPEND COMMON_AUDIO_SRC "${W}/common_audio/signal_processing/${n}.c")
endforeach()
list(APPEND COMMON_AUDIO_SRC "${W}/common_audio/signal_processing/dot_product_with_scale.cc")

add_library(webrtc_apm STATIC
    ${AEC3_SRC} ${AVX2_SRC} ${NS_SRC} ${AGC2_SRC} ${MISC_SRC} ${COMMON_AUDIO_SRC}
    ${STUBS_DIR}/default_task_queue_factory.cc
    ${STUBS_DIR}/rtc_event_log_null.cc
    ${STUBS_DIR}/matched_filter_avx2_stub.cc)

target_include_directories(webrtc_apm PUBLIC "${W}" "${ABSEIL_DIR}" "${CHROMIUM_TP}" "${STUBS_DIR}/include")
# WEBRTC_ENABLE_AVX2: lets rtc_base/cpu_info.cc detect AVX2 at runtime, so AEC3 and the AGC2 voice detector use
# their AVX2 kernels (the *_avx2.cc files below) on CPUs that have it and stay on SSE2 elsewhere.
target_compile_definitions(webrtc_apm PUBLIC WEBRTC_WIN NOMINMAX RTC_DISABLE_METRICS WEBRTC_APM_DEBUG_DUMP=0 WEBRTC_ENABLE_AVX2
                                       _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_compile_options(webrtc_apm PRIVATE /W0 /EHsc /bigobj /guard:cf)
set_source_files_properties(${AVX2_SRC} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
set_source_files_properties("${CHROMIUM_TP}/third_party/pffft/src/pffft.c" PROPERTIES COMPILE_DEFINITIONS "_USE_MATH_DEFINES")
set_source_files_properties("${W}/system_wrappers/source/metrics.cc" PROPERTIES COMPILE_DEFINITIONS "WIN32_LEAN_AND_MEAN")
target_link_libraries(webrtc_apm PUBLIC absl::strings absl::str_format absl::int128 absl::base
                                        absl::raw_logging_internal absl::algorithm
                                        winmm)   # timeGetTime (rtc_base/system_time.cc)
