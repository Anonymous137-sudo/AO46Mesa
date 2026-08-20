# SPDX-License-Identifier: MIT

ao46mtl_backend_path = get_option('ao46mtl-backend-path')
ao46mtl_include_path = get_option('ao46mtl-include-path')

if ao46mtl_backend_path == '' or ao46mtl_include_path == ''
  error('ao46mtl requires -Dao46mtl-backend-path and -Dao46mtl-include-path')
endif

dep_ao46mtl_backend = cc.find_library(
  'AO46MesaMetalBackend',
  dirs : [ao46mtl_backend_path],
  required : true,
)

libao46mtl = static_library(
  'ao46mtl',
  files('ao46mtl_screen.c', 'ao46mtl_screen.h'),
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux,
    include_directories(ao46mtl_include_path),
  ],
  dependencies : [dep_ao46mtl_backend],
  gnu_symbol_visibility : 'hidden',
)

driver_ao46mtl = declare_dependency(
  link_with : [libao46mtl],
  dependencies : [dep_ao46mtl_backend],
)
