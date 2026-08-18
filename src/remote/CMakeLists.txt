include_directories(${DARKTABLE_BINDIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

add_executable(darktable-remote-worker
  remote_main.c
  remote_protocol.c
  remote_session.c
  remote_exposure.c
  remote_surface.c)

set_target_properties(darktable-remote-worker PROPERTIES LINKER_LANGUAGE C)
target_include_directories(darktable-remote-worker PRIVATE ${JSON_GLIB_INCLUDE_DIRS})
target_link_directories(darktable-remote-worker PRIVATE ${JSON_GLIB_LIBRARY_DIRS})
target_link_libraries(darktable-remote-worker lib_darktable whereami ${JSON_GLIB_LIBRARIES})

# Preserve the full source commit in capabilities/state digests. Git archives
# expand PROJECT_VERSION to archive-<40 hex>; release tarballs may not carry it.
set(REMOTE_DARKTABLE_COMMIT "")
if(NOT SOURCE_PACKAGE)
  execute_process(COMMAND git rev-parse HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE REMOTE_DARKTABLE_COMMIT
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
elseif(PROJECT_VERSION MATCHES "^archive-([0-9a-fA-F]+)$")
  set(REMOTE_DARKTABLE_COMMIT "${CMAKE_MATCH_1}")
endif()
string(LENGTH "${REMOTE_DARKTABLE_COMMIT}" REMOTE_DARKTABLE_COMMIT_LENGTH)
if(NOT REMOTE_DARKTABLE_COMMIT_LENGTH EQUAL 40
   OR NOT REMOTE_DARKTABLE_COMMIT MATCHES "^[0-9a-fA-F]+$")
  set(REMOTE_DARKTABLE_COMMIT "unknown")
endif()
target_compile_definitions(darktable-remote-worker
                           PRIVATE DT_REMOTE_DARKTABLE_COMMIT="${REMOTE_DARKTABLE_COMMIT}")

if(WIN32)
  _detach_debuginfo(darktable-remote-worker bin)
else()
  set_target_properties(darktable-remote-worker
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                        RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif()

install(TARGETS darktable-remote-worker
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT DTApplication)
