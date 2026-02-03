
#ifndef RDMA_RESOURCES_H_
#define RDMA_RESOURCES_H_

#include <infiniband/verbs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* - qp parameters - */
#define QP_MAX_SEND_WR 512
#define QP_MAX_RECV_WR 64
#define QP_MAX_SEND_SGE 16
#define QP_MAX_RECV_SGE 16

/* - ibv_wc parameters / capability - */
#define QP_MAX_POLL_WC 64

/* - cq capability / size - */
#define CQ_MAX_SIZE ((QP_MAX_SEND_WR + QP_MAX_RECV_WR) * 3 / 2)

/* - when ibv_poll_cq(), the max waiting time - */
#define MAX_POLL_CQ_TIMEOUT 2000

/* - the max time of LRU using qp time - */
#define MAX_QP_REUSE_TIMEOUT 2000

/* - rdma_cm: rdma_listen(res->cm_id,RDMA_LISTEN_BACKLOG) - */
#define RDMA_LISTEN_BACKLOG 512

/* - 同步模式：用于指定 Server 端如何准备接收同步信号 - */
typedef enum {
  RDMA_SYNC_NONE = 0, /* 无同步，仅使用 RDMA Write */
  RDMA_SYNC_CDC = 1,  /* 使用 CDC 同步 (Write + Send) */
  RDMA_SYNC_IMME = 2  /* 使用 Write with Immediate 同步 */
} rdma_sync_mode_t;

/* - 操作类型，write/read/send -
        receive的话可以在改变qp状态的时候默认准备一个 -
*/
typedef enum {
  RDMA_OP_SEND = 0,
  RDMA_OP_READ = 1,
  RDMA_OP_WRITE = 2,
  RDMA_OP_WRITE_WITH_IMM = 3 /* Write with Immediate for synchronization */
} rdma_trans_op_t;

/* 用于保存rdma传输所必需的配置变量 |  这些参数来自系统的环境变量和用户定义(比如
 * -p -t 等等这种参数 ) - */
typedef struct {
  char *dev_name;    /* IB device name */
  char *server_name; /* server host name */
  uint32_t tcp_port; /* server TCP port */
  int ib_port;
  int gid_idx;                /* gid index to use */
  int traffic_class;          /* tclass of lossless network */
  int use_rdma_cm;            /* 0=use socket, 1=use rdma_cm */
  rdma_sync_mode_t sync_mode; /* 同步模式: NONE/CDC/IMME */
} config_t;

/* - 用户想要发送数据，需要填充的数据，填充示例：

    // 数据1：字符串
    char data1[] = "First message: Hello from RDMA batch";
    rdma_trans_sge_t sge1 = {
        .buf = data1,
        .buf_len = strlen(data1) + 1
    };

    // 数据2：二进制数据
    struct batch_data {
        int id;
        float values[4];
        char tag[16];
    } data2 = {
        .id = 1001,
        .values = {1.1f, 2.2f, 3.3f, 4.4f},
        .tag = "BATCH_TEST"
    };
    rdma_trans_sge_t sge2 = {
        .buf = &data2,
        .buf_len = sizeof(data2)
    };

    // 数据3：Scatter-Gather数据
    char sg_part1[] = "[SG1] First segment";
    char sg_part2[] = "[SG2] Second segment with more data";
    char sg_part3[] = "[SG3] Final segment";

    rdma_trans_sge_t sges3[3] = {
        {.buf = sg_part1, .buf_len = strlen(sg_part1) + 1},
        {.buf = sg_part2, .buf_len = strlen(sg_part2) + 1},
        {.buf = sg_part3, .buf_len = strlen(sg_part3) + 1}
    };

    // WR1：单SGE文本消息
    rdma_trans_wr_t wr1 = {
        .op_type = RDMA_OP_WRITE,
        .sges = &sge1,
        .sge_count = 1,
        .wr_len = sge1.buf_len
    };

    // WR2：单SGE二进制数据
    rdma_trans_wr_t wr2 = {
        .op_type = RDMA_OP_WRITE,
        .sges = &sge2,
        .sge_count = 1,
        .wr_len = sge2.buf_len
    };

    // WR3：多SGE Scatter-Gather
    rdma_trans_wr_t wr3 = {
        .op_type = RDMA_OP_WRITE,
        .sges = sges3,
        .sge_count = 3,
        .wr_len = sges3[0].buf_len + sges3[1].buf_len + sges3[2].buf_len
    };
*/
typedef struct {
  void *buf;
  size_t buf_len;
  struct ibv_mr *mr;
} rdma_trans_sge_t; // - 需要确保所有的sge的mr在同一个pd下 -

typedef struct {
  rdma_trans_op_t op_type; // - rdma的操作类型 -
  rdma_trans_sge_t *sges;  // - 一个mr可能有多个sge -
  int sge_count;           // - 一个mr所包含的sge的数量 -
  size_t wr_len;           // - 一个mr中所有sge的buff_len的长度 -
  uint32_t imm_data; // - 当 op_type == RDMA_OP_WRITE_WITH_IMM 时使用的立即数 -

} rdma_trans_wr_t; // - 这是给用户使用的，让其填写的想要传输的数据的内容 -

#endif

