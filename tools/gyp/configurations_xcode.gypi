# Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

# The purpose of this file and others in this directory is to simulate
# the Chromium build enviroment. This file is included in all GYP
# files that are used by the Dart project. This includes V8's GYP
# files.

# READ BEFORE EDITING:
# Do not add Dart VM specific configuration to this file. Instead,
# modify runtime/tools/gyp/runtime-configurations.gypi.

{
  'variables': {
    'xcode_gcc_version%': '<!(python <(DEPTH)/tools/gyp/find_mac_gcc_version.py)',
  },
  'target_defaults': {
    'configurations': {
      'Dart_Base': {
        'abstract': 1,
        'xcode_settings': {
          'GCC_VERSION': '<(xcode_gcc_version)',
          'GCC_C_LANGUAGE_STANDARD': 'ansi',
          'GCC_ENABLE_CPP_EXCEPTIONS': 'NO', # -fno-exceptions
          'GCC_ENABLE_CPP_RTTI': 'NO', # -fno-rtti
          'GCC_DEBUGGING_SYMBOLS': 'default', # -g
          'GCC_GENERATE_DEBUGGING_SYMBOLS': 'YES', # Do not strip symbols
          'GCC_SYMBOLS_PRIVATE_EXTERN': 'YES', # -fvisibility=hidden
          'GCC_INLINES_ARE_PRIVATE_EXTERN': 'YES', # -fvisibility-inlines-hidden
          'GCC_WARN_NON_VIRTUAL_DESTRUCTOR': 'YES', # -Wnon-virtual-dtor
          'GCC_TREAT_WARNINGS_AS_ERRORS': 'YES', # -Werror
          'WARNING_CFLAGS': [
            '<@(common_gcc_warning_flags)',
            '-Wtrigraphs', # Disable Xcode default.
            '-Wreturn-type',
            '-Werror=return-type',
          ],

          # Generate PIC code as Chrome is switching to this.
          'GCC_DYNAMIC_NO_PIC': 'NO',

          # When searching for header files, do not search
          # subdirectories. Without this option, vm/assert.h conflicts
          # with the system header assert.h. Apple also recommend
          # setting this to NO.
          'ALWAYS_SEARCH_USER_PATHS': 'NO',

          # Attempt to remove compiler options that Xcode adds by default.
          'GCC_CW_ASM_SYNTAX': 'NO', # Remove -fasm-blocks.

          'GCC_ENABLE_PASCAL_STRINGS': 'NO',
          'GCC_ENABLE_TRIGRAPHS': 'NO',
          'PREBINDING': 'NO',
        },
      },
    },
  },
}
