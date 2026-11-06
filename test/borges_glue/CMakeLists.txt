set(BORGES_GLUE_DIR ${REPO_ROOT}/src/borges/logic)
add_executable(borges_glue
  GlueStateTest.cpp
  ${BORGES_GLUE_DIR}/GlueState.cpp
  ${BORGES_GLUE_DIR}/LegacyMigration.cpp
  ${BORGES_GLUE_DIR}/PrivatePaths.cpp
  SourceGuardsTest.cpp
)
target_compile_definitions(borges_glue PRIVATE BORGES_REPO_ROOT="${REPO_ROOT}")
target_include_directories(borges_glue PRIVATE ${BORGES_GLUE_DIR} ${REPO_ROOT}/lib/BorgesCore)
target_link_libraries(borges_glue PRIVATE borges_core_lib GTest::gtest_main)
target_compile_options(borges_glue PRIVATE -Wall -Wextra)
gtest_discover_tests(borges_glue DISCOVERY_TIMEOUT 60)
