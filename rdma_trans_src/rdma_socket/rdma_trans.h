
#ifndef RDMA_TRANS_H
#define RDMA_TRANS_H

#include "../rdma_cdc/rdma_cdc.h"
#include "../rdma_resources.h"
#include <infiniband/verbs.h>
#include <sys/time.h>

#define RDMA_CDC_RECV_SLOTS 8

/* - 建链时交换数据的结构体 - */
// - 每个sge想要传输的数据 -
struct cm_mr_data_t {
  uint32_t rkey;
  uint64_t addr;
  size_t len;
} __attribute__((packed));

struct cm_con_data_ex_t {
  uint16_t lid;
  uint8_t gid[16];
  uint32_t qp_num; // - 建链之间就已经知道qp_num，也就是准备好创建qp -

  struct cm_mr_data_t mrs;
} __attribute__((packed));

// - 创建qp，以及初始化qp时，所需要的所有资源 结构体 (不需要用户自定义的)-
typedef struct {
  bool is_alive;
  // int in_wr_num;
  int sock;
  /* TCP socket file descriptor */ // -
                                   // 想了半天，sock也是qp的一部分，目前还是把sock放在res中，如果后续有多qp的话，可能多个qp会复用一个sock，这才解耦出来
  struct timeval start_time;       /* 记录qp最近使用的时间，创建的时间也算 */
  struct ibv_device_attr device_attr; /* Device attributes */
  struct ibv_port_attr port_attr;     /* IB port attributes */
  struct ibv_context *ib_ctx;         /* device handle */
  struct ibv_pd *pd;                  /* PD handle */
  struct ibv_cq *cq;                  /* CQ handle */
  struct ibv_qp *qp;                  /* QP handle */

  struct ibv_mr
      *cdc_mr; /* - msg的mr放在了rdma_trans_wr_t中，cdc的mr放在create_qp_res_t中
                  - */
  rdma_cdc_t *cdc_send_buf; /* CDC发送缓冲，即使多个slot，也共用一个cdc_send_buf */
  rdma_cdc_t **cdc_recv_bufs; // 多流水线，每个slot都需要一个cdc_receive_buf
  int cdc_recv_slots;
  int cdc_recv_post_idx;
  int cdc_recv_cons_idx;
  rdma_cdc_t *cdc_last_consumed;

  struct cm_con_data_ex_t *remote_props; // - 对端传输过来的的建链信息。

  /* ========== 异步完成通知支持 ========== */
  struct ibv_comp_channel *comp_channel; /* 完成事件通道 */
  int epoll_fd;                          /* epoll 文件描述符 */
  bool async_mode;                       /* 是否启用异步模式 */
} create_qp_res_t;

// void print_config(config_t* config);

// - 检查ibv 上下文context 是不是正常的 -
int rdma_context_init(create_qp_res_t *res, config_t *config);

// int mr_create(create_qp_res_t *res, rdma_trans_wr_t* msg, config_t* config);
int qp_create(create_qp_res_t *res, int max_send_wr, int max_recv_wr,
              int max_send_sge, int max_recv_sge);

int resources_create(create_qp_res_t *res, rdma_trans_wr_t *msg,
                     config_t *config);

int sock_established(create_qp_res_t *res, rdma_trans_wr_t *msg,
                     config_t *config);

int pd_create(create_qp_res_t *res);

int cq_create(create_qp_res_t *res, int cq_size);

/**
 * 创建支持异步事件通知的 CQ
 * @param res 资源结构
 * @param cq_size CQ 大小
 * @return 0 成功，非 0 失败
 */
int cq_create_async(create_qp_res_t *res, int cq_size);

void qp_destroy(create_qp_res_t *res);

void mr_destroy(rdma_trans_wr_t *msg);

void pd_destroy(create_qp_res_t *res);

void cq_destroy(create_qp_res_t *res);

/**
 * 等待并处理异步完成事件
 * @param res 资源结构
 * @param wc 输出的 WC 数组
 * @param max_wc 最大 WC 数量
 * @param timeout_ms 超时毫秒数 (-1 表示永久等待)
 * @return 完成的 WC 数量，-1 表示错误
 */
int rdma_async_wait_completion(create_qp_res_t *res, struct ibv_wc *wc,
                               int max_wc, int timeout_ms);

/**
 * 释放RDMA资源
 * @param qp_res：需要释放的上下文句柄
 * @return 成功返回RDMA_SUCCESS，失败返回错误码
 */
void rdma_trans_destroy(create_qp_res_t *qp_res, rdma_trans_wr_t *msg,
                        config_t *config);

/**
 * 初始化RDMA资源
 * @param qp_res：需要初始化的上下文句柄
 * @return 成功返回RDMA_SUCCESS，失败返回错误码
 */
int rdma_trans_init(create_qp_res_t *qp_res, rdma_trans_wr_t *msg,
                    config_t *config);

/**
 * 执行RDMA传输操作
 * @return 成功返回RDMA_SUCCESS，失败返回错误码（如RDMA_ERR_TIMEOUT）
 */
int rdma_trans_post(create_qp_res_t *qp_res, rdma_trans_wr_t *msg,
                    config_t *config);

/**
 * 执行RDMA completion操作
 * @return 成功返回RDMA_SUCCESS，失败返回错误码（如RDMA_ERR_TIMEOUT）
 */
int rdma_trans_completion(create_qp_res_t *qp_res);

rdma_cdc_t *rdma_cdc_acquire_post_buf(create_qp_res_t *res);
rdma_cdc_t *rdma_cdc_consume_buf(create_qp_res_t *res);

#endif

