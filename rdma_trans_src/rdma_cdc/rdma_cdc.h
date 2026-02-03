/*
    - 本文件用于保存 在write操作后
   server无法感知数据是不是已经传输完成，而添加的同步机制，用于来实现
*/

#ifndef RDMA_CDC_H
#define RDMA_CDC_H

#include "../rdma_resources.h"

typedef struct {

  uint8_t client_is_write; /* -
                              完成write操作后，client通知server端client端已经完成本次write操作了，1表示通知
                              - */
  uint8_t
      server_is_confirmed; /* -
                              server端接受到client端的client_is_write==1后，并且处理好数据后，向client端回复的*/

  uint32_t qp_num;

  struct ibv_mr *mr;

} rdma_cdc_t;

int post_receive_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr);

int post_send_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr);

/**
 * 等待 CDC 同步完成 (等待收到对端的 RECV 信号)
 * @param cq CQ句柄
 * @return 0成功，非0失败
 */
int rdma_cdc_completion(struct ibv_cq *cq);

#endif
