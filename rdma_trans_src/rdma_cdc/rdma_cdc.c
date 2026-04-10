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

int post_receive_cdc_ex(void *cdc_addr, struct ibv_qp *qp, struct ibv_mr *mr,
                        uint64_t wr_id) {
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
  rr.wr_id = wr_id; /* 使用传入的 wr_id 标识 slot */
  rr.sg_list = &sge;
  rr.num_sge = 1;

  /* post the Receive Request to the RQ */
  rc = ibv_post_recv(qp, &rr, &bad_wr);
  if (rc) {
    fprintf(stderr, "rdma_cdc_ex: failed to post RR (wr_id=%lu)\n", wr_id);
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
      IBV_SEND_SIGNALED |
      IBV_SEND_INLINE; /* Signaled + Inline：
                        * Inline 在 post 时即拷贝数据到 WQE，不依赖后续 NIC DMA
                        * 读取 buffer， 防止 4-pipeline 下后续 slot 修改 cdc
                        * buffer 覆盖前一个未发出的 CDC 数据。 rdma_cdc_t 仅约
                        * 16 字节，远小于 inline 阈值(通常 256B)。*/

  /* there is a Receive Request in the responder side, so we won't get any
   * into RNR flow */
  rc = ibv_post_send(qp, &sr, &bad_wr);
  if (rc) {
    fprintf(stderr, "failed to post SR\n");
  } else {
    fprintf(stdout, "rdma_cdc: Send Request was posted\n");
  }

  return rc;
}

int rdma_cdc_completion(struct ibv_cq *cq) {
  struct ibv_wc wc;
  int poll_result;

  /* 单条轮询避免批量 poll 导致 RECV completion 被丢弃 */
  while (1) {
    poll_result = ibv_poll_cq(cq, 1, &wc);

    if (poll_result < 0) {
      fprintf(stderr, "poll CQ failed in rdma_cdc_completion\n");
      return 1;
    }
    if (poll_result == 0) {
      continue;
    }

    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "bad cdc status: 0x%x, opcode: %d\n", wc.status,
              wc.opcode);
      return 1;
    }

    /* 通过 opcode 识别：只有收到对端的信号 (RECV) 才返回成功 */
    if (wc.opcode == IBV_WC_RECV) {
      fprintf(stderr, "rdma_cdc_completion: RECV wr_id=%lu\n", wc.wr_id);
      return 0;
    }
  }
}

