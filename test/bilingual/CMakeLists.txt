add_executable(BilingualTest BilingualTest.cpp)

target_link_libraries(BilingualTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(BilingualTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
)

gtest_discover_tests(BilingualTest)

# The tooltip end to end (chapter parse, pairing, page matching) against stub Page/renderer/storage.
add_executable(BilingualTooltipTest
  TooltipTest.cpp
  ${REPO_ROOT}/src/bilingual/BilingualTooltip.cpp
  ${REPO_ROOT}/src/bilingual/SentencePairing.cpp
  ${REPO_ROOT}/src/bilingual/SentenceSplitter.cpp
  ${REPO_ROOT}/src/bilingual/TextNormalize.cpp
)

target_include_directories(BilingualTooltipTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tooltip_stubs
  ${REPO_ROOT}/src
)

target_link_libraries(BilingualTooltipTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(BilingualTooltipTest)
