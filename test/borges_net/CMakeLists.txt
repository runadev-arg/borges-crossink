FetchContent_GetProperties(arduinojson)
set(BORGES_NET_DIR ${REPO_ROOT}/lib/BorgesCore/net)

add_library(borges_net_lib STATIC
  ${BORGES_NET_DIR}/ClockChain.cpp
  ${BORGES_NET_DIR}/DnsChain.cpp
  ${BORGES_NET_DIR}/DnsCodec.cpp
  ${BORGES_NET_DIR}/HttpDate.cpp
  ${BORGES_NET_DIR}/NetFailure.cpp
  ${BORGES_NET_DIR}/NetRecords.cpp
)
target_link_libraries(borges_net_lib PUBLIC borges_core_lib)
target_compile_options(borges_net_lib PRIVATE -Wall -Wextra -pedantic)

add_executable(borges_net NetTest.cpp)
target_include_directories(borges_net PRIVATE ${REPO_ROOT}/test/borges_core ${arduinojson_SOURCE_DIR}/src)
target_link_libraries(borges_net PRIVATE borges_net_lib GTest::gtest_main)
target_compile_options(borges_net PRIVATE -Wall -Wextra)
gtest_discover_tests(borges_net DISCOVERY_TIMEOUT 60)
