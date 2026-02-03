project('scp_rdma', 'c', version: '1.0', default_options: ['c_std=c99'])

# 定义公共编译选项
common_cflags = [
  '-Wall',
  '-Wextra',
  '-O2',
  '-g',
  '-D_POSIX_C_SOURCE=200809L'
]

# 定义依赖库
rdma_deps = [
  dependency('librdmacm'),
  dependency('libibverbs')
]

# 引用现有RDMA库的源文件（路径相对于当前文件夹）
lib_dir = '../rdma_trans_src'
lib_sources = [
  lib_dir / 'rdma_socket/rdma_trans.c',
  lib_dir / 'rdma_cdc/rdma_cdc.c'
]

# scp_rdma 自己的源文件
scp_sources = [
  'scp_rdma_main.c',
  'scp_rdma_sender.c',
  'scp_rdma_receiver.c'
]

# 头文件包含路径
inc_dirs = include_directories('.', lib_dir, lib_dir / 'rdma_socket', lib_dir / 'rdma_cdc')

# 生成 scp_rdma 可执行文件
executable('scp_rdma',
  sources: scp_sources + lib_sources,
  include_directories: inc_dirs,
  c_args: common_cflags,
  dependencies: rdma_deps,
  install: true
)

