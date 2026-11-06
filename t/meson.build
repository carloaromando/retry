test_sources = [ 'retry.test.cpp'
               , 'retry.property.cpp'
               ]

test_deps = [gtest_main, retry_dep]

if with_asio
  test_sources += 'async_retry.test.cpp'
  test_deps += async_retry_dep
endif

testexe = executable('retry-test'
                    , test_sources
                    , dependencies : test_deps
                    )

test('retry test', testexe)
