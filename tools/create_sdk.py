#!/usr/bin/env python
#
# Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
#
# A script which will be invoked from gyp to create an SDK.
#
# Usage: create_sdk.py sdk_directory
#
# The SDK will be used either from the command-line or from the editor.
# Top structure is
#
# ..dart-sdk/
# ....bin/
# ......dart or dart.exe (executable)
# ......dart.lib (import library for VM native extensions on Windows)
# ......dart2js
# ......dart_analyzer
# ......pub
# ......snapshots/
# ........utils_wrapper.dart.snapshot
# ....include/
# ......dart_api.h
# ......dart_debugger_api.h
# ....lib/
# ......_internal/
# ......async/
# ......collection/
# ......_collection_dev/
# ......core/
# ......crypto/
# ......html/
# ......io/
# ......isolate/
# ......json/
# ......math/
# ......mirrors/
# ......uri/
# ......utf/
# ......typeddata/
# ....packages/
# ......args/
# ......intl/
# ......logging/
# ......meta/
# ......serialization
# ......unittest/
# ......(more will come here)
# ....util/
# ......analyzer/
# ........dart_analyzer.jar
# ......dartanalyzer/
# ........dartanalyzer.jar
# ........(third-party libraries for dart_analyzer)
# ......pub/
# ......(more will come here)


import glob
import optparse
import os
import re
import sys
import subprocess
import tempfile
import utils

HOST_OS = utils.GuessOS()

# TODO(dgrove): Only import modules following Google style guide.
from os.path import basename, dirname, join, realpath, exists, isdir

# TODO(dgrove): Only import modules following Google style guide.
from shutil import copyfile, copymode, copytree, ignore_patterns, rmtree, move


def GetOptions():
  options = optparse.OptionParser(usage='usage: %prog [options]')
  options.add_option("--sdk_output_dir",
      help='Where to output the sdk')
  options.add_option("--utils_snapshot_location",
      help='Location of the utils snapshot.')
  return options.parse_args()


def ReplaceInFiles(paths, subs):
  '''Reads a series of files, applies a series of substitutions to each, and
     saves them back out. subs should by a list of (pattern, replace) tuples.'''
  for path in paths:
    contents = open(path).read()
    for pattern, replace in subs:
      contents = re.sub(pattern, replace, contents)

    dest = open(path, 'w')
    dest.write(contents)
    dest.close()


def Copy(src, dest):
  copyfile(src, dest)
  copymode(src, dest)


def CopyShellScript(src_file, dest_dir):
  '''Copies a shell/batch script to the given destination directory. Handles
     using the appropriate platform-specific file extension.'''
  file_extension = ''
  if HOST_OS == 'win32':
    file_extension = '.bat'

  src = src_file + file_extension
  dest = join(dest_dir, basename(src_file) + file_extension)
  Copy(src, dest)


def CopyDartScripts(home, sdk_root):
  # TODO(dgrove) - add pub once issue 6619 is fixed
  for executable in ['dart2js', 'dartanalyzer', 'dartdoc']:
    CopyShellScript(os.path.join(home, 'sdk', 'bin', executable),
                    os.path.join(sdk_root, 'bin'))


def CopySnapshots(snapshot, sdk_root):
  copyfile(snapshot, join(sdk_root, 'bin', 'snapshots', basename(snapshot)))


def Main(argv):
  # Pull in all of the gypi files which will be munged into the sdk.
  HOME = dirname(dirname(realpath(__file__)))

  (options, args) = GetOptions()

  SDK = options.sdk_output_dir
  SDK_tmp = '%s.tmp' % SDK

  SNAPSHOT = options.utils_snapshot_location

  # TODO(dgrove) - deal with architectures that are not ia32.

  if exists(SDK):
    rmtree(SDK)

  if exists(SDK_tmp):
    rmtree(SDK_tmp)

  os.makedirs(SDK_tmp)

  # Create and populate sdk/bin.
  BIN = join(SDK_tmp, 'bin')
  os.makedirs(BIN)

  os.makedirs(join(BIN, 'snapshots'))

  # Copy the Dart VM binary and the Windows Dart VM link library
  # into sdk/bin.
  #
  # TODO(dgrove) - deal with architectures that are not ia32.
  build_dir = os.path.dirname(SDK)
  dart_file_extension = ''
  analyzer_file_extension = ''
  if HOST_OS == 'win32':
    dart_file_extension = '.exe'
    analyzer_file_extension = '.bat'
    dart_import_lib_src = join(HOME, build_dir, 'dart.lib')
    dart_import_lib_dest = join(BIN, 'dart.lib')
    copyfile(dart_import_lib_src, dart_import_lib_dest)
  dart_src_binary = join(HOME, build_dir, 'dart' + dart_file_extension)
  dart_dest_binary = join(BIN, 'dart' + dart_file_extension)
  copyfile(dart_src_binary, dart_dest_binary)
  copymode(dart_src_binary, dart_dest_binary)
  # Strip the binaries on platforms where that is supported.
  if HOST_OS == 'linux':
    subprocess.call(['strip', dart_dest_binary])
  elif HOST_OS == 'macos':
    subprocess.call(['strip', '-x', dart_dest_binary])

  # Copy analyzer into sdk/bin
  ANALYZER_HOME = join(HOME, build_dir, 'analyzer')
  dart_analyzer_src_binary = join(ANALYZER_HOME, 'bin',
      'dart_analyzer' + analyzer_file_extension)
  dart_analyzer_dest_binary = join(BIN,
      'dart_analyzer' + analyzer_file_extension)
  copyfile(dart_analyzer_src_binary, dart_analyzer_dest_binary)
  copymode(dart_analyzer_src_binary, dart_analyzer_dest_binary)

  # Create pub shell script.
  # TODO(dgrove) - delete this once issue 6619 is fixed
  pub_src_script = join(HOME, 'utils', 'pub', 'sdk', 'pub')
  CopyShellScript(pub_src_script, BIN)

  #
  # Create and populate sdk/include.
  #
  INCLUDE = join(SDK_tmp, 'include')
  os.makedirs(INCLUDE)
  copyfile(join(HOME, 'runtime', 'include', 'dart_api.h'),
           join(INCLUDE, 'dart_api.h'))
  copyfile(join(HOME, 'runtime', 'include', 'dart_debugger_api.h'),
           join(INCLUDE, 'dart_debugger_api.h'))

  #
  # Create and populate sdk/lib.
  #

  LIB = join(SDK_tmp, 'lib')
  os.makedirs(LIB)

  #
  # Create and populate lib/{core, crypto, isolate, json, uri, utf, ...}.
  #

  os.makedirs(join(LIB, 'html'))

  for library in ['_internal',
                  'async', 'collection', '_collection_dev', 'core',
                  'crypto', 'io', 'isolate',
                  join('chrome', 'dart2js'), join('chrome', 'dartium'),
                  join('html', 'dart2js'), join('html', 'dartium'),
                  join('html', 'html_common'),
                  join('indexed_db', 'dart2js'), join('indexed_db', 'dartium'),
                  'json', 'math', 'mirrors', 'typeddata',
                  join('svg', 'dart2js'), join('svg', 'dartium'),
                  'uri', 'utf',
                  join('web_audio', 'dart2js'), join('web_audio', 'dartium'),
                  join('web_gl', 'dart2js'), join('web_gl', 'dartium'),
                  join('web_sql', 'dart2js'), join('web_sql', 'dartium')]:
    copytree(join(HOME, 'sdk', 'lib', library), join(LIB, library),
             ignore=ignore_patterns('*.svn', 'doc', '*.py', '*.gypi', '*.sh'))

  # Create and copy packages.
  PACKAGES = join(SDK_tmp, 'packages')
  os.makedirs(PACKAGES)

  #
  # Create and populate packages/{args, intl, logging, meta, unittest, ...}
  #

  for library in ['args', 'http', 'intl', 'logging', 'meta', 'oauth2', 'pathos',
                  'serialization', 'unittest', 'yaml', 'analyzer_experimental']:

    copytree(join(HOME, 'pkg', library, 'lib'), join(PACKAGES, library),
             ignore=ignore_patterns('*.svn'))

  # Create and copy tools.
  UTIL = join(SDK_tmp, 'util')
  os.makedirs(UTIL)

  # Create and copy Analyzer library into 'util'
  ANALYZER_DEST = join(UTIL, 'analyzer')
  os.makedirs(ANALYZER_DEST)

  analyzer_src_jar = join(ANALYZER_HOME, 'util', 'analyzer',
                          'dart_analyzer.jar')
  analyzer_dest_jar = join(ANALYZER_DEST, 'dart_analyzer.jar')
  copyfile(analyzer_src_jar, analyzer_dest_jar)

  jarsToCopy = [ join("args4j", "2.0.12", "args4j-2.0.12.jar"),
                 join("guava", "r13", "guava-13.0.1.jar"),
                 join("json", "r2_20080312", "json.jar") ]
  for jarToCopy in jarsToCopy:
    dest_dir = join (ANALYZER_DEST, os.path.dirname(jarToCopy))
    os.makedirs(dest_dir)
    dest_file = join (ANALYZER_DEST, jarToCopy)
    src_file = join(ANALYZER_HOME, 'util', 'analyzer', jarToCopy)
    copyfile(src_file, dest_file)

  # Create and copy dartanalyzer into 'util'
  DARTANALYZER_SRC = join(HOME, build_dir, 'dartanalyzer')
  DARTANALYZER_DEST = join(UTIL, 'dartanalyzer')
  os.makedirs(DARTANALYZER_DEST)
  
  jarFiles = glob.glob(join(DARTANALYZER_SRC, '*.jar'))
  
  for jarFile in jarFiles:
    copyfile(jarFile, join(DARTANALYZER_DEST, os.path.basename(jarFile)))
  
  # Create and populate util/pub.
  copytree(join(HOME, 'utils', 'pub'), join(UTIL, 'pub'),
           ignore=ignore_patterns('.svn', 'sdk'))

  # Copy in 7zip for Windows.
  if HOST_OS == 'win32':
    copytree(join(HOME, 'third_party', '7zip'),
             join(join(UTIL, 'pub'), '7zip'),
             ignore=ignore_patterns('.svn'))

  ReplaceInFiles([
      join(UTIL, 'pub', 'io.dart'),
    ], [
      ("../../third_party/7zip/7za.exe",
       "7zip/7za.exe"),
    ])

  # Copy dart2js/dartdoc/pub.
  CopyDartScripts(HOME, SDK_tmp)
  CopySnapshots(SNAPSHOT, SDK_tmp)

  # Write the 'version' file
  version = utils.GetVersion()
  versionFile = open(os.path.join(SDK_tmp, 'version'), 'w')
  versionFile.write(version + '\n')
  versionFile.close()

  # Write the 'revision' file
  revision = utils.GetSVNRevision()

  if revision is not None:
    with open(os.path.join(SDK_tmp, 'revision'), 'w') as f:
      f.write(revision + '\n')
      f.close()

  Copy(join(HOME, 'README.dart-sdk'), join(SDK_tmp, 'README'))

  move(SDK_tmp, SDK)

if __name__ == '__main__':
  sys.exit(Main(sys.argv))
