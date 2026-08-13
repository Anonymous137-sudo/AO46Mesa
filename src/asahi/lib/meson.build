# Copyright 2018 Rob Clark
# Copyright 2019 Collabora, Ltd.
# Copyright 2017 Intel Corporation
# SPDX-License-Identifier: MIT

libasahi_lib_files = files(
  'agx_bo.c',
  'agx_border.c',
  'agx_linker.c',
  'agx_bg_eot.c',
  'agx_tilebuffer.c',
  'agx_nir_lower_msaa.c',
  'agx_nir_lower_sample_intrinsics.c',
  'agx_nir_lower_tilebuffer.c',
  'agx_nir_lower_uvs.c',
  'agx_nir_lower_vbo.c',
  'agx_nir_prolog_epilog.c',
  'agx_ppp.h',
  'agx_scratch.c',
  'agx_submit.c',
  'agx_va.c',
  'pool.c',
)

# The upstream device and virtio implementations speak the Linux DRM UABI.
# Keep them intact for KMS/DRM builds, but do not compile them into a Darwin
# archive while the macOS AGX user-client implementation is developed.
if system_has_kms_drm
  libasahi_lib_files += files(
    'agx_device.c',
    'agx_device_virtio.c',
  )
endif

libasahi_decode_files = files(
  'decode.c',
)

libasahi_decode = static_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  link_with : [libagx2_disasm],
  build_by_default : false,
)

libasahi_decode_shared = shared_library(
  'asahi_decode',
  [libasahi_decode_files, agx_pack],
  dependencies : [dep_valgrind, idep_mesautil],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  build_by_default : with_tools.contains('asahi'),
  link_with : [libagx2_disasm],
)

libagx_shaders = custom_target(
  input : libagx_spv,
  output : ['libagx_shaders.h', 'libagx_shaders.c'],
  command : [prog_asahi_clc, libagx_spv, '@OUTPUT0@', '@OUTPUT1@'],
)

idep_libagx_shaders_h = declare_dependency(
  sources : [libagx_shaders],
  include_directories : include_directories('.'),
)

libasahi_lib_link_with = [libasahi_decode, libpoly_nir]
libasahi_lib_deps = [dep_valgrind, idep_nir, idep_mesautil, idep_libagx,
                     idep_libpoly]
if system_has_kms_drm
  libasahi_lib_link_with += libvdrm
  libasahi_lib_deps += dep_libdrm
endif

libasahi_lib = static_library(
  'asahi_lib',
  [libasahi_lib_files, libagx_shaders, agx_pack, sha1_h],
  include_directories : [inc_asahi, inc_virtio_gpu, inc_virtio_vdrm],
  c_args : [no_override_init_args, '-Wno-c2x-extensions'],
  gnu_symbol_visibility : 'hidden',
  link_with: libasahi_lib_link_with,
  dependencies: libasahi_lib_deps,
  build_by_default : false,
)

if dep_iokit.found() and dep_corefoundation.found() and host_machine.system() == 'darwin'
  libasahi_macos_winsys = static_library(
    'asahi_macos_winsys',
    files('agx_macos_device.c', 'agx_macos_uabi.c', 'agx_macos_bo.c',
          'agx_macos_shader_provenance.c', 'agx_macos_queue.c',
          'agx_macos_command.c', 'agx_macos_submission_observation.c',
          'agx_macos_submission_build.c', 'agx_macos_mesa_device.c',
          'agx_macos_submission_lease.c',
          'agx_macos_iosurface.m',
          'agx_macos_screen_bootstrap.c'),
    include_directories : [inc_src, inc_include, inc_asahi, inc_virtio_gpu,
                           inc_virtio_vdrm, include_directories('../libagx')],
    dependencies : [dep_iokit, dep_corefoundation, idep_nir_headers,
                    dependency('appleframeworks', modules : ['Foundation', 'IOSurface'])],
    gnu_symbol_visibility : 'hidden',
    build_by_default : false,
  )

  executable(
    'asahi_macos_winsys_probe',
    'tests/agx_macos_device_test.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_uabi_contract_smoke',
    'tests/agx_macos_uabi_contract_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_device_session_state_smoke',
    'tests/agx_macos_device_session_state_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_bo_smoke',
    'tests/agx_macos_bo_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_queue_smoke',
    'tests/agx_macos_queue_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_queue_lifecycle_smoke',
    'tests/agx_macos_queue_lifecycle_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_command_smoke',
    'tests/agx_macos_command_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_submission_observation_smoke',
    'tests/agx_macos_submission_observation_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_submission_build_smoke',
    'tests/agx_macos_submission_build_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_mesa_device_smoke',
    'tests/agx_macos_mesa_device_smoke.c',
    include_directories : [inc_src, inc_include, inc_asahi, inc_virtio_gpu,
                           inc_virtio_vdrm, include_directories('../libagx')],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : [libasahi_macos_winsys, libasahi_lib],
    build_by_default : false,
  )

  executable(
    'asahi_macos_shader_provenance_smoke',
    'tests/agx_macos_shader_provenance_smoke.c',
    include_directories : [inc_src, inc_include, inc_asahi,
                           include_directories('../libagx')],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'agx_submit_info_smoke',
    'tests/agx_submit_info_smoke.c',
    include_directories : [inc_src, inc_include, inc_asahi, inc_virtio_gpu,
                           inc_virtio_vdrm, include_directories('../libagx')],
    link_with : libasahi_lib,
    build_by_default : false,
  )

  executable(
    'asahi_macos_submission_lease_smoke',
    'tests/agx_macos_submission_lease_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_iosurface_smoke',
    'tests/agx_macos_iosurface_smoke.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Foundation', 'IOSurface']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_screen_bootstrap_smoke',
    'tests/agx_macos_screen_bootstrap_smoke.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [dep_iokit, dep_corefoundation,
                    dependency('appleframeworks', modules : ['Foundation', 'IOSurface'])],
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_trace_control',
    'tests/agx_macos_trace_control.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_shader_allocation_trace',
    'tests/agx_macos_shader_allocation_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_shader_execution_trace',
    'tests/agx_macos_shader_execution_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_resource_record_variation_trace',
    'tests/agx_macos_resource_record_variation_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_resource_record_range_trace',
    'tests/agx_macos_resource_record_range_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_compute_resource_record_trace',
    'tests/agx_macos_compute_resource_record_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_public_buffer_binding_trace',
    'tests/agx_macos_public_buffer_binding_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_compute_resource_range_trace',
    'tests/agx_macos_compute_resource_range_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_render_resource_record_trace',
    'tests/agx_macos_render_resource_record_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_render_resource_lifecycle_trace',
    'tests/agx_macos_render_resource_lifecycle_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_render_descriptor_variation_trace',
    'tests/agx_macos_render_descriptor_variation_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_iosurface_drawable_trace',
    'tests/agx_macos_iosurface_drawable_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation', 'IOSurface']),
    link_with : libasahi_macos_winsys,
    build_by_default : false,
  )

  executable(
    'asahi_macos_queue_lifecycle_trace',
    'tests/agx_macos_queue_lifecycle_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )

  executable(
    'asahi_macos_command_buffer_lifecycle_trace',
    'tests/agx_macos_command_buffer_lifecycle_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )

  executable(
    'asahi_macos_command_buffer_pair_lifecycle_trace',
    'tests/agx_macos_command_buffer_pair_lifecycle_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )

  executable(
    'asahi_macos_multi_queue_command_buffer_lifecycle_trace',
    'tests/agx_macos_multi_queue_command_buffer_lifecycle_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )

  executable(
    'asahi_macos_empty_submission_trace',
    'tests/agx_macos_empty_submission_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )

  executable(
    'asahi_macos_two_queue_empty_submission_trace',
    'tests/agx_macos_two_queue_empty_submission_trace.m',
    include_directories : [inc_src, inc_asahi],
    dependencies : dependency('appleframeworks', modules : ['Metal', 'Foundation']),
    build_by_default : false,
  )
endif

if with_tests
  test(
    'asahi_submit_info_smoke',
    executable(
      'asahi_submit_info_smoke',
      'tests/agx_submit_info_smoke.c',
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
      dependencies: [idep_mesautil],
      link_with : libasahi_lib,
    ),
    suite : ['asahi'],
  )

  test(
    'libasahi_tests',
    executable(
      'libasahi_tests',
      files(
        'tests/test-packing.cpp',
        'tests/test-tilebuffer.cpp',
      ),
      c_args : [c_msvc_compat_args, no_override_init_args],
      gnu_symbol_visibility : 'hidden',
      dependencies: [idep_gtest, idep_agx_pack, idep_mesautil],
      link_with : [libasahi_lib, libasahi_layout],
    ),
    suite : ['asahi'],
    protocol : 'gtest',
  )
endif

if dep_iokit.found()
  libasahi_wrap = shared_library(
    'wrap',
    'wrap.c', 'agx_macos_submission_observation.c',
    include_directories : [inc_src, inc_asahi],
    dependencies : [idep_mesautil, dep_iokit],
    c_args : [no_override_init_args, '-Wno-missing-prototypes'],
    gnu_symbol_visibility : 'hidden',
    build_by_default : with_tools.contains('asahi'),
    link_with: libasahi_decode,
  )
endif
