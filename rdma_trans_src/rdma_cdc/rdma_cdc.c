#include "rdma_cdc.h"
#include <stdio.h>

int post_receive_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr) {
  struct ibv_recv_wr rr;
  struct ibv_sge sge;
  struct ibv_recv_wr *bad_wr;
  int rc;

  /* prepare the scatter/gather entry */
  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)cdc_addr;
  sge.length = sizeof(rdma_cdc_t);
  sge.lkey = mr->lkey;

  /* prepare the receive work request */
  memset(&rr, 0, sizeof(rr));
  rr.next = NULL;
  rr.wr_id = 0;
  rr.sg_list = &sge;
  rr.num_sge = 1;

  /* post the Receive Request to the RQ */
  rc = ibv_post_recv(qp, &rr, &bad_wr);
  if (rc) {
    fprintf(stderr, "rdma_cdc: failed to post RR\n");
  } else {
    fprintf(stdout, "rdma_cdc: Receive Request was posted\n");
  }

  return rc;
}

int post_send_cdc(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr) {

  int rc = 0;

  struct ibv_send_wr sr;
  struct ibv_sge sge;
  struct ibv_send_wr *bad_wr = NULL;

  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)cdc_addr;
  sge.length = sizeof(rdma_cdc_t);
  sge.lkey = mr->lkey;

  /* prepare the send work request */
  memset(&sr, 0, sizeof(sr));
  sr.next = NULL;
  sr.wr_id =
      0; // -
         // 目前是默认置0，其实可以让用户指定(修改msg的结构体)，以及res设置一个全局的值(修改res的结构体)
         // -
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = IBV_WR_SEND;
  sr.send_flags =
      IBV_SEND_SIGNALED; /* 恢复 Signaled 发送，确保 SQ 空间能被回收 */

  /* there is a Receive Request in the responder side, so we won't get any into
   * RNR flow */
  rc = ibv_post_send(qp, &sr, &bad_wr);
  if (rc) {
    fprintf(stderr, "failed to post SR\n");
  } else {
    fprintf(stdout, "rdma_cdc: Send Request was posted\n");
  }

  return rc;
}

int rdma_cdc_completion(struct ibv_cq *cq) {
  struct ibv_wc wc[10];
  int poll_result;
  int i;

  /* 自动排水 + 批量轮询 (Self-Draining Batch Polling)
   * 循环直到抓到 RECV (对端的命令)，期间自动吸收并丢弃所有本端的成功报告 (SEND)
   */
  while (1) {
    poll_result = ibv_poll_cq(cq, 10, wc);

    if (poll_result < 0) {
      fprintf(stderr, "poll CQ failed in rdma_cdc_completion\n");
      return 1;
    }

    for (i = 0; i < poll_result; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr, "bad cdc status: 0x%x, opcode: %d\n", wc[i].status,
                wc[i].opcode);
        return 1;
      }

      /* 通过 opcode 识别：只有收到对端的信号 (RECV) 才返回成功 */
      if (wc[i].opcode == IBV_WC_RECV) {
        return 0;
      }
    }
  }
}
