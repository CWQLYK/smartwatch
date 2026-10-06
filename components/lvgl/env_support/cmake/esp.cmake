file(GLOB_RECURSE SOURCES ${LVGL_ROOT_DIR}/src/*.c)

idf_build_get_property(LV_MICROPYTHON LV_MICROPYTHON)

if(LV_MICROPYTHON)
  idf_component_register(
    SRCS
    ${SOURCES}
    INCLUDE_DIRS
    ${LVGL_ROOT_DIR}
    ${LVGL_ROOT_DIR}/src
    ${LVGL_ROOT_DIR}/../
    REQUIRES
    main)
else()
  if(CONFIG_LV_BUILD_EXAMPLES)
    file(GLOB_RECURSE EXAMPLE_SOURCES ${LVGL_ROOT_DIR}/examples/*.c)
    set_source_files_properties(${EXAMPLE_SOURCES} COMPILE_FLAGS "-Wno-unused-variable -Wno-format")
  endif()

  if(CONFIG_LV_USE_DEMO_WIDGETS)
    file(GLOB_RECURSE DEMO_WIDGETS_SOURCES ${LVGL_ROOT_DIR}/demos/widgets/*.c)
    list(APPEND DEMO_SOURCES ${DEMO_WIDGETS_SOURCES})
  endif()
  if(CONFIG_LV_USE_DEMO_KEYPAD_AND_ENCODER)
    file(GLOB_RECURSE DEMO_KEYPAD_AND_ENCODER_SOURCES ${LVGL_ROOT_DIR}/demos/keypad_encoder/*.c)
    list(APPEND DEMO_SOURCES ${DEMO_KEYPAD_AND_ENCODER_SOURCES})
  endif()
  if(CONFIG_LV_USE_DEMO_BENCHMARK)
    file(GLOB_RECURSE DEMO_BENCHMARK_SOURCES ${LVGL_ROOT_DIR}/demos/benchmark/*.c)
    list(APPEND DEMO_SOURCES ${DEMO_BENCHMARK_SOURCES})
  endif()
  if(CONFIG_LV_USE_DEMO_STRESS)
    file(GLOB_RECURSE DEMO_STRESS_SOURCES ${LVGL_ROOT_DIR}/demos/stress/*.c)
    list(APPEND DEMO_SOURCES ${DEMO_STRESS_SOURCES})
  endif()
  if(CONFIG_LV_USE_DEMO_MUSIC)
    file(GLOB_RECURSE DEMO_MUSIC_SOURCES ${LVGL_ROOT_DIR}/demos/music/*.c)
    list(APPEND DEMO_SOURCES ${DEMO_MUSIC_SOURCES})
    set_source_files_properties(${DEMO_MUSIC_SOURCES} COMPILE_FLAGS "-Wno-format")
  endif()

  # ------------------------------------------------------------------
  # 本地修改（智能手表项目，2026-10-06）：
  #   原文件**无条件**把 ${LVGL_ROOT_DIR}/examples 和 /demos 加进 INCLUDE_DIRS，
  #   而这两个目录我们为了给仓库瘦身已经删掉了（它们只是示例资源，共 67MB，
  #   不参与固件编译）。目录不存在时 CMake 会直接报
  #   "Include directory ... is not a directory"。
  #   所以这里改成"存在才加"。
  #
  #   ⚠️ 以后从上游更新 LVGL 时，这个改动会被覆盖，需要重新打一次。
  # ------------------------------------------------------------------
  set(LVGL_LOCAL_INCLUDE_DIRS
      ${LVGL_ROOT_DIR}
      ${LVGL_ROOT_DIR}/src
      ${LVGL_ROOT_DIR}/../
  )
  if(EXISTS ${LVGL_ROOT_DIR}/examples)
    list(APPEND LVGL_LOCAL_INCLUDE_DIRS ${LVGL_ROOT_DIR}/examples)
  endif()
  if(EXISTS ${LVGL_ROOT_DIR}/demos)
    list(APPEND LVGL_LOCAL_INCLUDE_DIRS ${LVGL_ROOT_DIR}/demos)
  endif()

  idf_component_register(SRCS ${SOURCES} ${EXAMPLE_SOURCES} ${DEMO_SOURCES}
      INCLUDE_DIRS ${LVGL_LOCAL_INCLUDE_DIRS}
      REQUIRES esp_timer)
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLV_CONF_INCLUDE_SIMPLE")

if(CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM)
  target_compile_definitions(${COMPONENT_LIB}
                             PUBLIC "-DLV_ATTRIBUTE_FAST_MEM=IRAM_ATTR")
endif()
