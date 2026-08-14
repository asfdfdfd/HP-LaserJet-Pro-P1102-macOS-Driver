project('p1102', 'c',
  version: '1.1.0',
  default_options: ['c_std=c11', 'warning_level=2', 'buildtype=release'])

cc = meson.get_compiler('c')
add_project_arguments('-mmacosx-version-min=13.0', '-Wno-unused-parameter',
                      language: 'c')

# ---------------------------------------------------------------------------
# Libraries / frameworks
# ---------------------------------------------------------------------------

libcups = cc.find_library('cups')
libcupsimage = cc.find_library('cupsimage')
threads = dependency('threads')
iokit = declare_dependency(
  link_args: ['-framework', 'IOKit', '-framework', 'CoreFoundation'])

# ---------------------------------------------------------------------------
# Vendored ZjStream engine + jbigkit
# ---------------------------------------------------------------------------

zjs_src = files(
  'src/zjs/vendor/foo2zjs.c',
  'src/zjs/vendor/jbig.c',
  'src/zjs/vendor/jbig_ar.c',
)
zjs_inc = include_directories('src/zjs')

ews_src = files('status/ews.c')
ews_inc = include_directories('status')

bundle_dir = '/Library/Printers/HP/p1102raster.bundle/Contents/MacOS'

# ---------------------------------------------------------------------------
# Driver binaries
# ---------------------------------------------------------------------------

rastertozjs = executable('rastertozjs',
  'src/rastertozjs.c', zjs_src,
  include_directories: zjs_inc,
  dependencies: [libcups, libcupsimage, threads],
  install: true,
  install_dir: bundle_dir)

p1102status = executable('p1102status',
  'status/p1102status.c', ews_src,
  include_directories: ews_inc,
  dependencies: iokit,
  install: true,
  install_dir: bundle_dir)

commandtozjs = executable('commandtozjs',
  'status/commandtozjs.c', ews_src, zjs_src,
  include_directories: [ews_inc, zjs_inc],
  dependencies: iokit,
  install: true,
  install_dir: bundle_dir)

ewsproxy = executable('ewsproxy',
  'status/ewsproxy.c', ews_src,
  include_directories: ews_inc,
  dependencies: [iokit, threads],
  install: true,
  install_dir: bundle_dir)

p1102cmd = executable('p1102cmd',
  'src/p1102cmd.c',
  dependencies: libcups,
  install: true,
  install_dir: bundle_dir)

usbprobe = executable('usbprobe',
  'status/usbprobe.c',
  dependencies: iokit,
  build_by_default: false)

# ---------------------------------------------------------------------------
# Install: bundle layout, Info.plist, PPD, ad-hoc codesign
# ---------------------------------------------------------------------------

install_data('bundle/Info.plist',
  install_dir: '/Library/Printers/HP/p1102raster.bundle/Contents')

install_data('ppd/HP-LaserJet-Pro-P1102.ppd',
  install_dir: '/Library/Printers/PPDs/Contents/Resources',
  rename: 'HP LaserJet Professional P1102.ppd')

meson.add_install_script('tools/codesign.py')

# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------

mkraster = executable('mkraster',
  'tests/mkraster.c',
  dependencies: [libcups, libcupsimage],
  build_by_default: false)

refmain = executable('refmain',
  'tests/refmain.c', zjs_src,
  include_directories: zjs_inc,
  dependencies: [libcups, libcupsimage, threads],
  build_by_default: false)

jbverify = executable('jbverify',
  'tests/jbverify.c',
  'src/zjs/vendor/jbig.c', 'src/zjs/vendor/jbig_ar.c',
  include_directories: include_directories('src/zjs/vendor'),
  build_by_default: false)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

python3 = import('python').find_installation()
check = files('tests/check.py')
check_quality = files('tests/check_quality.py')

REFARGS = '-z2 -P -L0 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1'

test('byte-identity-letter600', python3,
  args: [check, 'letter600',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--pbm', 'test600.pbm',
         '--refargs', REFARGS])

test('byte-identity-noise1200', python3,
  args: [check, 'noise1200',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--pbm', 'testnoise.pbm',
         '--raster-args', '-r 1200x600',
         '--refargs', '-z2 -P -L0 -r1200x600 -g10200x6600 -p1 -m1 -s7 -n1'])

test('byte-identity-twopages', python3,
  args: [check, 'twopages',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--pbm', 'two.pbm',
         '--refargs', REFARGS])

test('byte-identity-duplex-longedge', python3,
  args: [check, 'duplex2',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--pbm', 'two.pbm',
         '--opts', 'Duplex=DuplexTumble',
         '--refargs', '-z2 -P -L0 -d4 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1'])

test('byte-identity-duplex-shortedge', python3,
  args: [check, 'duplex3',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--pbm', 'two.pbm',
         '--opts', 'Duplex=DuplexNoTumble',
         '--refargs', '-z2 -P -L0 -d5 -r600x600 -g5100x6600 -p1 -m1 -s7 -n1'])

test('byte-identity-gray-w8', python3,
  args: [check, 'gray',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--raster-pbm', 'gray.pgm', '--ref-pbm', 'gray_thr.pbm',
         '--raster-args=-g',
         '--opts', 'Halftone=Threshold',
         '--refargs', REFARGS])

test('byte-identity-gray-k8', python3,
  args: [check, 'grayK',
         '--filter', rastertozjs, '--refmain', refmain, '--mkraster', mkraster,
         '--raster-pbm', 'gray_K.pgm', '--ref-pbm', 'gray_thr.pbm',
         '--raster-args=-K',
         '--opts', 'Halftone=Threshold',
         '--refargs', REFARGS])

test('selftest', python3,
  args: [check_quality, 'selftest',
         '--cmdtozjs', commandtozjs, '--jbverify', jbverify])

test('diffuse-auto', python3,
  args: [check_quality, 'diffuse',
         '--filter', rastertozjs, '--mkraster', mkraster, '--jbverify', jbverify])
