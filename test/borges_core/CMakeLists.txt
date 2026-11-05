FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)

set(BORGES_CORE_DIR ${REPO_ROOT}/lib/BorgesCore)

add_library(borges_core_lib STATIC
  ${BORGES_CORE_DIR}/Api.cpp
  ${BORGES_CORE_DIR}/AtomicFile.cpp
  ${BORGES_CORE_DIR}/Catalog.cpp
  ${BORGES_CORE_DIR}/Codec.cpp
  ${BORGES_CORE_DIR}/Events.cpp
  ${BORGES_CORE_DIR}/JsonStream.cpp
  ${BORGES_CORE_DIR}/Outbox.cpp
  ${BORGES_CORE_DIR}/PairingFlow.cpp
  ${BORGES_CORE_DIR}/ResumePolicy.cpp
  ${BORGES_CORE_DIR}/Spool.cpp
  ${BORGES_CORE_DIR}/Store.cpp
  ${BORGES_CORE_DIR}/SyncEngine.cpp
)
target_include_directories(borges_core_lib PUBLIC ${BORGES_CORE_DIR})
target_compile_options(borges_core_lib PRIVATE -Wall -Wextra -pedantic)
if(BORGES_CORE_WERROR)
  target_compile_options(borges_core_lib PRIVATE -Werror)
endif()

add_executable(borges_core
  AtomicFileTest.cpp
  CatalogTest.cpp
  CodecTest.cpp
  CrashMatrixTest.cpp
  DesignRulesTest.cpp
  EventsTest.cpp
  FinalizeTest.cpp
  JsonStreamTest.cpp
  OutboxTest.cpp
  PairingFlowTest.cpp
  ResumePolicyTest.cpp
  StoreTest.cpp
  SyncEngineTest.cpp
)
target_include_directories(borges_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${arduinojson_SOURCE_DIR}/src)
target_link_libraries(borges_core PRIVATE borges_core_lib GTest::gtest_main)
target_compile_options(borges_core PRIVATE -Wall -Wextra)
gtest_discover_tests(borges_core DISCOVERY_TIMEOUT 60)
