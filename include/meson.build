retry_inc = include_directories('.')

headers = ['retry/retry.hpp']

retry_dep = declare_dependency(include_directories : retry_inc)

# Lets a parent project write dependency('retry') instead of reaching into
# this subproject for a variable.
meson.override_dependency('retry', retry_dep)

if with_asio
  headers += 'retry/async_retry.hpp'

  async_retry_dep = declare_dependency(include_directories : retry_inc
                                      , dependencies : asio_dep
                                      )

  meson.override_dependency('retry-async', async_retry_dep)
endif

install_headers(headers, subdir : 'retry')
