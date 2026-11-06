project('retry'
        , 'cpp'
        , default_options : [ 'cpp_std=c++23'
                            , 'warning_level=3'  # -Wall -Wextra -Wpedantic
                            , 'werror=true'      # override with -Dwerror=false
                            ]
        , version : '0.1.0'
        , license : 'BSD3'
        , meson_version : '>= 1.1.0'
        )

asio_dep = dependency('asio'
                      , required : get_option('asio')
                      , allow_fallback : true
                      )
with_asio = asio_dep.found()

summary({'asio (async_retry)' : with_asio}, section : 'Features')

subdir('include')

# A project that pulls this in as a subproject wants the headers, not our
# test suite.
if get_option('tests') and not meson.is_subproject()
  gtest = dependency('gtest')
  gtest_main = dependency('gtest_main')

  subdir('t')
endif
