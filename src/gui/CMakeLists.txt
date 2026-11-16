pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24)
pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

add_executable(booru-grabber
  main.cpp
  widgets.cpp
  style.cpp
  thumbnails.cpp
  mainwindow.cpp
  searchtab.cpp
  viewer.cpp
  videoplayer.cpp
  downloadstab.cpp
  favoritestab.cpp
  monitorstab.cpp
  logtab.cpp
  sitesdialog.cpp
  settingsdialog.cpp
  dialogs.cpp
  tray.cpp)
target_compile_options(booru-grabber PRIVATE -Wall -Wextra)
target_link_libraries(booru-grabber PRIVATE booru-grabber-core PkgConfig::GTK3)
add_dependencies(booru-grabber sites)

if(GST_FOUND)
  target_link_libraries(booru-grabber PRIVATE PkgConfig::GST)
  target_compile_definitions(booru-grabber PRIVATE BG_HAVE_GSTREAMER=1)
  message(STATUS "Video playback: GStreamer ${GST_gstreamer-1.0_VERSION}")
else()
  message(STATUS "Video playback: disabled (GStreamer not found)")
endif()

if(BOORU_GRABBER_TRAY)
  pkg_check_modules(APPINDICATOR IMPORTED_TARGET ayatana-appindicator3-0.1)
  if(APPINDICATOR_FOUND)
    target_link_libraries(booru-grabber PRIVATE PkgConfig::APPINDICATOR)
    target_compile_definitions(booru-grabber PRIVATE BG_HAVE_TRAY=1)
    message(STATUS "Tray icon: libayatana-appindicator")
  else()
    message(STATUS "Tray icon: disabled (libayatana-appindicator not found)")
  endif()
endif()

install(TARGETS booru-grabber RUNTIME DESTINATION ${BG_BINDIR})
