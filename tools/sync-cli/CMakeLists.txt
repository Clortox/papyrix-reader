cmake_minimum_required(VERSION 3.16)
project(sync-cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

find_package(CURL REQUIRED)

# The sync core is the firmware's own, so both CLIs behave exactly like the
# device. Only the transport (libcurl) and the article store (filesystem) are
# host specific.
set(SYNC_CORE_SOURCES
  ${PROJECT_ROOT}/lib/FreshRss/src/FreshRssConfig.cpp
  ${PROJECT_ROOT}/lib/FreshRss/src/FreshRssJson.cpp
  ${PROJECT_ROOT}/lib/FreshRss/src/FreshRssHttp.cpp
  ${PROJECT_ROOT}/lib/FreshRss/src/FreshRssArticle.cpp
  ${PROJECT_ROOT}/lib/FreshRss/src/FreshRssSync.cpp
  ${PROJECT_ROOT}/src/apps/RssState.cpp
)

# Zotero core, added once the sync orchestrator lands.
set(ZOTERO_SOURCES
  ${PROJECT_ROOT}/lib/Zotero/src/ZoteroConfig.cpp
  ${PROJECT_ROOT}/lib/Zotero/src/ZoteroItems.cpp
)
set(ZOTERO_AVAILABLE FALSE)
if(EXISTS ${PROJECT_ROOT}/lib/Zotero/src/ZoteroSync.cpp)
  set(ZOTERO_AVAILABLE TRUE)
  list(APPEND ZOTERO_SOURCES ${PROJECT_ROOT}/lib/Zotero/src/ZoteroSync.cpp)
endif()

include_directories(
  ${PROJECT_ROOT}/lib/FreshRss/src
  ${PROJECT_ROOT}/lib/Zotero/src
  ${PROJECT_ROOT}/src/apps
)

add_executable(rss-sync rss_sync_main.cpp SyncCli.cpp ${SYNC_CORE_SOURCES})
target_link_libraries(rss-sync PRIVATE CURL::libcurl)
set_target_properties(rss-sync PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(ZOTERO_AVAILABLE)
  add_executable(zotero-sync zotero_sync_main.cpp SyncCli.cpp ${SYNC_CORE_SOURCES} ${ZOTERO_SOURCES})
  target_link_libraries(zotero-sync PRIVATE CURL::libcurl)
  set_target_properties(zotero-sync PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
