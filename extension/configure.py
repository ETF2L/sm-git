# vim: set ts=2 sw=2 tw=99 et:
# Copyright 2026 ETF2L Contributors, Dave
#
# Configures the AMBuild build of the git extension, for example:
#   python configure.py --sm-path=/build/sourcemod --deps-path=/build/deps --targets=x86,x86_64 --enable-optimize
import sys

try:
  from ambuild2 import run
except ImportError:
  sys.stderr.write('AMBuild 2.2 or later must be installed to build this extension.\n')
  sys.exit(1)

parser = run.BuildParser(sourcePath=sys.path[0], api='2.2')
parser.options.add_argument('--sm-path', type=str, dest='sm_path', default=None,
                            help='Path to SourceMod (with its submodules)')
parser.options.add_argument('--deps-path', type=str, dest='deps_path', default=None,
                            help='Path to the static libraries, with <arch>/include and <arch>/lib (deps/build-deps.sh)')
parser.options.add_argument('--enable-debug', action='store_const', const='1', dest='debug',
                            help='Enable debugging symbols')
parser.options.add_argument('--enable-optimize', action='store_const', const='1', dest='opt',
                            help='Enable optimization')
parser.options.add_argument('--targets', type=str, dest='targets', default='x86,x86_64',
                            help='Target architectures, separated by commas')
parser.Configure()
