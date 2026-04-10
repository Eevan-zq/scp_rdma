/*
    - 本文件用于保�?在write操作�?   server无法感知数据是不是已经传输完成，而添加的同步机制，用于来实现
*/

#ifndef RDMA_CDC_H
#define RDMA_CDC_H

#include "../rdma_resources.h"

typedef struct {

  uint8_t
      client_is_write; /* -
                          完成write操作后，client通知server端client端已经完成本次write操作了，1表示通知
                          - */
  uint8_t
      server_is_confirmed; /* -
                              server端接受到client端的client_is_write==1后，并且处理好数据后，向client端回复的*/

  uint32_t qp_num;

  /* 双缓冲扩展字�?*/
  uint8_t slot_id;     /* 当前使用的槽�?ID (0 �?1) */
  uint8_t signal_type; /* 信号类型: 0=DATA_READY, 1=ACK */
  uint16_t credits;    /* peer CDC RECV credits */

  struct ibv_mr *mr;

} rdma_cdc_t;

int post_receive_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr);

/**
 * 发布 CDC 接收请求（带 wr_id 用于双缓冲识别）
 * @param cdc_addr CDC 缓冲区地址
 * @param qp QP 句柄
 * @param mr MR 句柄
 * @param wr_id 工作请求 ID（用于标识是哪个 slot �?ACK�? */
int post_receive_cdc_ex(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr,
                        uint64_t wr_id);

int post_send_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr);

/**
 * 等待 CDC 同步完成 (等待收到对端�?RECV 信号)
 * @param cq CQ句柄
 * @return 0成功，非0失败
 */
int rdma_cdc_completion(struct ibv_cq *cq);

#endif



