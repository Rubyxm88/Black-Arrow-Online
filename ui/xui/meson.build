xemu_ss.add(files(
  'actions.cc',
  'animation.cc',
  'compat.cc',
  'debug.cc',
  'font-manager.cc',
  'gl-helpers.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'xbcheck.cc',
  'shader-cache.cc',
  'guide.cc',
  'game-cache.cc',
  'versions.cc',
  'notifications.cc',
  'perf-overlay.cc',
  'popup-menu.cc',
  'reporting.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'snapshot-manager.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
))

xemu_ss.add([curl, files('github-issue.cc')])

if host_os == 'windows'
  xemu_ss.add(files('win32-dxgi-present.cc'))
endif

if host_os in ['darwin', 'linux']
  xemu_ss.add(files('self-update.cc', 'full-report.cc'))
endif
