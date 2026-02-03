/*
 * scp_rdma.h - SCP over RDMA 公共头文件
 *
 * 包含配置结构、上下文结构和公共函数声明
 */

#ifndef SCP_RDMA_H
#define SCP_RDMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 引用现有RDMA库的头文件 */
#include "../rdma_trans_src/rdma_cdc/rdma_cdc.h"
#include "../rdma_trans_src/rdma_resources.h"
#include "../rdma_trans_src/rdma_socket/rdma_trans.h"

/* 引用协议定义 */
#include "scp_rdma_protocol.h"

/* 默认配置 */
#define SCP_DEFAULT_PORT 1777
#define SCP_DEFAULT_IB_PORT 1
#define SCP_DEFAULT_GID_IDX 3
#define SCP_DEFAULT_DEV_NAME "xscale_2"

/* 进度回调函数类型 */
typedef void (*scp_progress_callback_t)(const char *filename,
                                        uint64_t transferred, uint64_t total);

/*
 * scp_rdma 配置结构
 */
typedef struct {
  char *dev_name;                      /* RDMA设备名称 */
  char *server_addr;                   /* 服务器地址（发送模式使用） */
  char *output_dir;                    /* 输出目录（接收模式使用） */
  uint32_t port;                       /* TCP端口号 */
  int ib_port;                         /* InfiniBand端口 */
  int gid_idx;                         /* GID索引 */
  bool recursive;                      /* 是否递归传输目录 */
  bool verbose;                        /* 详细输出 */
  bool is_server;                      /* 是否为服务端模式 */
  scp_progress_callback_t progress_cb; /* 进度回调 */
} scp_rdma_config_t;

/*
 * scp_rdma 上下文结构
 */
typedef struct {
  scp_rdma_config_t config; /* 配置 */
  config_t rdma_config;     /* RDMA库配置 */
  create_qp_res_t res;      /* RDMA资源 */
  scp_state_t state;        /* 当前状态 */

  /* 统计信息 */
  uint64_t total_bytes; /* 总传输字节数 */
  uint32_t total_files; /* 总文件数 */
  uint32_t total_dirs;  /* 总目录数 */

  /* 当前传输状态 */
  char current_file[SCP_MAX_PATH_LEN]; /* 当前正在传输的文件 */
  uint64_t current_file_size;          /* 当前文件大小 */
  uint64_t current_transferred;        /* 当前文件已传输字节 */

  /* 持久大会话优化 (Persistent Session) */
  bool session_established;  /* 是否已建立持久会话 */
  void *session_buf;         /* 预注册的会话缓冲区 (用于元数据和数据块汇聚) */
  struct ibv_mr *session_mr; /* 会话缓冲区的 MR */
  rdma_trans_sge_t *session_sge; /* 预分配的 SGE 缓存 */
} scp_rdma_context_t;

/*
 * 传输结果结构
 */
typedef struct {
  int status;                 /* 0=成功，非0=失败 */
  uint64_t bytes_transferred; /* 传输的字节数 */
  uint32_t files_transferred; /* 传输的文件数 */
  uint32_t dirs_transferred;  /* 传输的目录数 */
  double elapsed_time;        /* 耗时（秒） */
  double throughput;          /* 吞吐量 (MB/s) */
} scp_result_t;

/* ==================== 公共函数声明 ==================== */

/**
 * 初始化scp_rdma上下文
 * @param ctx  上下文指针
 * @param cfg  配置指针
 * @return 0成功，非0失败
 */
int scp_rdma_init(scp_rdma_context_t *ctx, const scp_rdma_config_t *cfg);

/**
 * 销毁scp_rdma上下文，释放资源
 * @param ctx 上下文指针
 */
void scp_rdma_destroy(scp_rdma_context_t *ctx);

/**
 * 获取默认配置
 * @param cfg 配置指针
 */
void scp_rdma_default_config(scp_rdma_config_t *cfg);

/* ==================== 发送端函数 ==================== */

/**
 * 发送单个文件
 * @param ctx         上下文
 * @param local_path  本地文件路径
 * @param remote_path 远程目标路径
 * @return 0成功，非0失败
 */
int scp_send_file(scp_rdma_context_t *ctx, const char *local_path,
                  const char *remote_path);

/**
 * 发送目录（递归）
 * @param ctx         上下文
 * @param local_path  本地目录路径
 * @param remote_path 远程目标路径
 * @return 0成功，非0失败
 */
int scp_send_directory(scp_rdma_context_t *ctx, const char *local_path,
                       const char *remote_path);

/**
 * 发送器主入口
 * @param ctx         上下文
 * @param local_path  本地路径（文件或目录）
 * @param remote_path 远程目标路径
 * @param result      传输结果（可选）
 * @return 0成功，非0失败
 */
int scp_sender_run(scp_rdma_context_t *ctx, const char *local_path,
                   const char *remote_path, scp_result_t *result);

/* ==================== 接收端函数 ==================== */

/**
 * 接收单个文件
 * @param ctx       上下文
 * @param meta      文件元数据
 * @return 0成功，非0失败
 */
int scp_receive_file(scp_rdma_context_t *ctx, const scp_file_meta_t *meta);

/**
 * 处理目录接收
 * @param ctx       上下文
 * @param dir_info  目录信息
 * @return 0成功，非0失败
 */
int scp_receive_directory(scp_rdma_context_t *ctx,
                          const scp_dir_info_t *dir_info);

/**
 * 接收器主入口 - 循环接收数据直到传输结束
 * @param ctx    上下文
 * @param result 传输结果（可选）
 * @return 0成功，非0失败
 */
int scp_receiver_run(scp_rdma_context_t *ctx, scp_result_t *result);

/* ==================== 工具函数 ==================== */

/**
 * 打印传输结果
 * @param result 结果指针
 */
void scp_print_result(const scp_result_t *result);

/**
 * 格式化字节大小为人类可读格式
 * @param bytes  字节数
 * @param buf    输出缓冲区
 * @param buflen 缓冲区长度
 * @return buf指针
 */
char *scp_format_size(uint64_t bytes, char *buf, size_t buflen);

/**
 * 默认进度显示回调
 */
void scp_default_progress(const char *filename, uint64_t transferred,
                          uint64_t total);

#endif /* SCP_RDMA_H */
