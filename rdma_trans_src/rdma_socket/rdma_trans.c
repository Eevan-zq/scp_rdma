
#define _GNU_SOURCE

#include "rdma_trans.h"

#include <arpa/inet.h>
#include <byteswap.h>
#include <endian.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

void sock_connect_exit(int listenfd, struct addrinfo *resolved_addr, int sockfd,
                       const char *servername) {

  if (listenfd) {
    close(listenfd);
  }

  if (resolved_addr) {
    freeaddrinfo(resolved_addr);
  }

  if (sockfd < 0) {
    if (servername) {
      fprintf(stderr, "Couldn't connect to %s\n", servername);
    } else {
      perror("server accept");
      fprintf(stderr, "accept() failed\n");
    }
  }
}

int sock_connect(const char *servername, int port) {
  struct addrinfo *resolved_addr = NULL;
  struct addrinfo *iterator;
  char service[6];
  int sockfd = -1;
  int listenfd = 0;
  int tmp;
  struct addrinfo hints = {
      .ai_flags = AI_PASSIVE, .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};

  if (sprintf(service, "%d", port) < 0) {
    sock_connect_exit(listenfd, resolved_addr, -1, servername);
  }

  /* Resolve DNS address, use sockfd as temp storage */
  sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
  if (sockfd < 0) {
    fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
    sock_connect_exit(listenfd, resolved_addr, sockfd, servername);
  }

  /* Search through results and find the one we want */
  for (iterator = resolved_addr; iterator; iterator = iterator->ai_next) {
    sockfd = socket(iterator->ai_family, iterator->ai_socktype,
                    iterator->ai_protocol);
    if (sockfd >= 0) {
      if (servername) {
        /* Client mode. Initiate connection to remote */
        if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen))) {
          fprintf(stderr, "failed connect \n");
          close(sockfd);
          sockfd = -1;
        }
      } else {
        /* Server mode. Set up listening socket an accept a connection */
        listenfd = sockfd;
        sockfd = -1;
        /* 设置 SO_REUSEADDR 以允许立即重用端口 */
        int opt = 1;
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
            0) {
          fprintf(stderr, "failed to set SO_REUSEADDR\n");
        }
        if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen)) {
          sock_connect_exit(listenfd, resolved_addr, sockfd, servername);
        }
        listen(listenfd, 1);
        sockfd = accept(listenfd, NULL, 0);
      }
    }
  }

  return sockfd;
}

int sock_sync_data(int sock, int xfer_size, char *local_data,
                   char *remote_data) {
  int rc = 0;
  int read_bytes = 0;
  int total_read_bytes = 0;
  rc = write(sock, local_data, xfer_size);

  if (rc < 0) {
    fprintf(stderr, "Failed writing data during sock_sync_data (errno=%d)\n",
            errno);
    return rc;
  } else if (rc < xfer_size) {
    fprintf(stderr, "Partial write during sock_sync_data (%d/%d)\n", rc,
            xfer_size);
    // 尝试继续写入剩余数据（简化处理，直接视为错误）
    // 这里我们直接返回错误
    return -1;
  } else {
    rc = 0;
  }

  while (!rc && total_read_bytes < xfer_size) {
    read_bytes = read(sock, remote_data + total_read_bytes,
                      xfer_size - total_read_bytes);
    if (read_bytes > 0) {
      total_read_bytes += read_bytes;
    } else if (read_bytes == 0) {
      fprintf(stderr, "Peer closed connection during sock_sync_data\n");
      rc = -1;
      break;
    } else {
      rc = read_bytes;
      break;
    }
  }
  return rc;
}

// -
// 异步的建链交换信息，需要client端先传输信息,然后server端根据这个信息来填充自己的信息，然后发送给client端，但是基本上就是知道长度就行，其他的都是根据长度来创建的
// -
// - 需要在创建mr之前（mr_create() ）将length数据交换过去 ，connect_qp()
// 又在mr_create() 之后， 好在 res->sockfd
// 是在resources_create()函数中就创建的，resources_create()在mr_create()和connect_qp()之前
// -
int sock_async_data(create_qp_res_t *res, rdma_trans_wr_t *msg,
                    config_t *config) {
  int rc = 0;
  int read_bytes = 0;
  size_t total_read_bytes = 0;
  size_t *remote_len = NULL;

  if (config->server_name) { // - client端 -

    size_t length = msg->wr_len;
    size_t written = 0;
    while (written < sizeof(size_t)) {
      rc = write(res->sock, ((char *)&length) + written,
                 sizeof(size_t) - written);
      if (rc < 0) {
        fprintf(
            stderr,
            "Failed writing length data during sock_async_data (errno=%d)\n",
            errno);
        return rc;
      }
      written += rc;
    }

    return 0;

  } else { // - server端 -
    if (msg->wr_len != 0) {
      return 0;
    }
    remote_len = (size_t *)malloc(sizeof(size_t));
    if (!remote_len) {
      rc = 1;
      return rc;
    }
    memset(remote_len, 0, sizeof(size_t));

    while (total_read_bytes < sizeof(size_t)) {
      read_bytes = read(res->sock, ((char *)remote_len) + total_read_bytes,
                        sizeof(size_t) - total_read_bytes);
      if (read_bytes > 0) {
        total_read_bytes += read_bytes;
      } else if (read_bytes == 0) {
        fprintf(stderr, "Peer closed connection during sock_async_data\n");
        free(remote_len);
        return -1;
      } else {
        fprintf(stderr, "Read error during sock_async_data (errno=%d)\n",
                errno);
        free(remote_len);
        return read_bytes;
      }
    }
    msg->wr_len = (*remote_len);

    free(remote_len);
    return 0;
  }
}

// - 后续是需要改进的，第一点是改成非阻塞的；第二点是 ibv_poll_cq(res->cq, 1,
// &wc)改为ibv_poll_cq(res->cq, 10, wc)，前提是 struct ibv_wr wc[10];
static int poll_completion(create_qp_res_t *res) {
  struct ibv_wc wc[10];
  unsigned long start_time_msec;
  unsigned long cur_time_msec;
  struct timeval cur_time;
  int poll_result;
  int i;

  /* poll the completion for a while before giving up of doing it .. */
  gettimeofday(&cur_time, NULL);
  start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);

  do {
    /* 批量 Poll (size=10) 以快速清理积压的 SEND 信号 */
    poll_result = ibv_poll_cq(res->cq, 10, wc);

    if (poll_result > 0) {
      for (i = 0; i < poll_result; i++) {
        if (wc[i].status != IBV_WC_SUCCESS) {
          fprintf(stderr, "got bad completion status: 0x%x, opcode: %d\n",
                  wc[i].status, wc[i].opcode);
          return 1;
        }

        /* 抓到任何发送相关的完成 (Write/Send/Read) 均视为本次同步请求的进展 */
        if (wc[i].opcode == IBV_WC_RDMA_WRITE || wc[i].opcode == IBV_WC_SEND ||
            wc[i].opcode == IBV_WC_RDMA_READ) {
          return 0;
        }
      }
    }

    gettimeofday(&cur_time, NULL);
    cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  } while (((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));

  fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
  return 1;
}

// -
// 这里后续也是要改进的，这里一个wr只有一个sge，后续得优化一下，万一一个msg->buff_len过大或者用户指定了多qp(这种情况得加一个qp_num的参数，不知道加到哪个结构体里呢)
// -
static int post_send(create_qp_res_t *res, rdma_trans_wr_t *msg) {
  struct ibv_send_wr sr;
  // struct ibv_sge sge;
  struct ibv_sge *sge_list;
  struct ibv_send_wr *bad_wr = NULL;
  int rc = 0, i = 0;

  sge_list = (struct ibv_sge *)malloc(msg->sge_count * sizeof(struct ibv_sge));
  if (!sge_list) {
    rc = 1;
    return rc;
  }

  for (i = 0; i < msg->sge_count; ++i) {

    sge_list[i].addr = (uintptr_t)msg->sges[i].buf;
    sge_list[i].length = msg->sges[i].buf_len;
    sge_list[i].lkey = msg->sges[i].mr->lkey;
  }
  /* prepare the send work request */
  memset(&sr, 0, sizeof(sr));
  sr.next = NULL;
  sr.wr_id =
      0; // -
         // 目前是默认置0，其实可以让用户指定(修改msg的结构体)，以及res设置一个全局的值(修改res的结构体)
         // -
  sr.sg_list = sge_list;
  sr.num_sge = msg->sge_count;
  if (msg->op_type == 0) {
    sr.opcode = IBV_WR_SEND;
  } else if (msg->op_type == 1) {
    sr.opcode = IBV_WR_RDMA_READ;
  } else if (msg->op_type == 2) {
    sr.opcode = IBV_WR_RDMA_WRITE;
  } else if (msg->op_type == 3) {
    sr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    sr.imm_data = htonl(msg->imm_data); /* Write with Immediate: 设置立即数 */
  } else {
    fprintf(stderr, "failed to post msg post_send \n");
    free(sge_list);
    rc = 1;
    return rc;
  }
  sr.send_flags = IBV_SEND_SIGNALED;
  if (sr.opcode != IBV_WR_SEND) {
    sr.wr.rdma.remote_addr = res->remote_props->mrs.addr;
    sr.wr.rdma.rkey = res->remote_props->mrs.rkey;
  }

  /* there is a Receive Request in the responder side, so we won't get any into
   * RNR flow */
  rc = ibv_post_send(res->qp, &sr, &bad_wr);
  if (rc) {
    fprintf(stderr, "failed to post SR\n");
  }

  free(sge_list);
  return rc;
}

__attribute__((unused)) static int post_receive(create_qp_res_t *res,
                                                rdma_trans_wr_t *msg) {
  struct ibv_recv_wr rr;
  struct ibv_sge *sge_list;
  struct ibv_recv_wr *bad_wr;
  int rc = 0, i = 0;

  sge_list = (struct ibv_sge *)malloc(msg->sge_count * sizeof(struct ibv_sge));
  if (!sge_list) {
    rc = 1;
    return rc;
  }

  for (i = 0; i < msg->sge_count; ++i) {

    sge_list[i].addr = (uintptr_t)msg->sges[i].buf;
    sge_list[i].length = msg->sges[i].buf_len;
    sge_list[i].lkey = msg->sges[i].mr->lkey;
  }

  /* prepare the receive work request */
  memset(&rr, 0, sizeof(rr));
  rr.next = NULL;
  rr.wr_id = 0;
  rr.sg_list = sge_list;
  rr.num_sge = msg->sge_count;

  /* post the Receive Request to the RQ */
  rc = ibv_post_recv(res->qp, &rr, &bad_wr);
  if (rc) {
    fprintf(stderr, "failed to post RR\n");
  }
  free(sge_list);
  return rc;
}

// - rdma_trans_wr_t 的初始化肯定不放在这里，用户定义好的 -
static void resources_init(create_qp_res_t *res) {
  memset(res, 0, sizeof *res);
  res->sock = -1;
  res->cdc_mr = NULL;
}

void context_res_exit(create_qp_res_t *res, int rc,
                      struct ibv_device **dev_list) {

  if (rc) {

    if (res->ib_ctx) {
      ibv_close_device(res->ib_ctx);
      res->ib_ctx = NULL;
    }
    if (dev_list) {
      ibv_free_device_list(dev_list);
      dev_list = NULL;
    }
  }
}

/*
    用于建立socket连接，这点之后需要改为io多路复用 -
*/
int sock_established(create_qp_res_t *res, rdma_trans_wr_t *msg,
                     config_t *config) {

  int rc = 0;

  if (res->sock <= 0) {
    /* if client side */
    if (config->server_name) {
      res->sock = sock_connect(config->server_name, config->tcp_port);
      if (res->sock < 0) {
        fprintf(stderr,
                "failed to establish TCP connection to server %s, port %d\n",
                config->server_name, config->tcp_port);
        rc = 1;
        goto sock_exit;
      }
    } else {
      fprintf(stdout, "waiting on port %d for TCP connection\n",
              config->tcp_port);
      res->sock = sock_connect(NULL, config->tcp_port);
      if (res->sock < 0) {
        fprintf(stderr,
                "failed to establish TCP connection with client on port %d\n",
                config->tcp_port);
        rc = 1;

        goto sock_exit;
      }
    }
  }

  // - 这样的话，server端的msg length就有了 -
  rc = sock_async_data(res, msg, config);

  if (rc) {
    fprintf(stderr, "sock_async_data failed \n");
    goto sock_exit;
  }

  // 成功，不关闭socket，直接返回
  return 0;

sock_exit:
  if (res->sock >= 0) {
    if (close(res->sock)) {
      fprintf(stderr, "failed to close socket\n");
    }
    res->sock = -1;
  }
  return rc;
}

int pd_create(create_qp_res_t *res) {

  int rc = 0;

  res->pd = ibv_alloc_pd(res->ib_ctx);
  if (!res->pd) {
    fprintf(stderr, "ibv_alloc_pd failed\n");
    rc = 1;
    pd_destroy(res);
    return rc;
  }
  return rc;
}

void pd_destroy(create_qp_res_t *res) {

  if (res->pd) {
    if (ibv_dealloc_pd(res->pd)) {
      fprintf(stderr, "failed to deallocate PD\n");
    }
    res->pd = NULL;
  }
}

int cq_create(create_qp_res_t *res, int cq_size) {

  int rc = 0;

  res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
  if (!res->cq) {
    fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
    rc = 1;
    cq_destroy(res);
    return rc;
  }

  return rc;
}

void cq_destroy(create_qp_res_t *res) {

  if (res->cq) {
    if (ibv_destroy_cq(res->cq)) {
      fprintf(stderr, "failed to destroy CQ\n");
    }
    res->cq = NULL;
  }
}

// -
// 说的很对，如果使用sync同步的方式来交换建链信息的话，那么创建mr一定是在交换建链信息之前，不然怎么交换呢；
// 但是如果不是这种同步的交换建链信息，针对不同的操作类型
// -        比如 write
// 操作，肯定是client端先准备好地址，然后来写到对端，现在就是两点：1是
// server端提前准备好了(这点最简单，下面的代码就可以用)；另一种就是server端没有提前准备好，这就需要非同步的来交换建链信息了
// - 也就是client先给server端自己的建链信息，然后server端再来创建buf和mr等等

// - 下面就是简单点：1是
// server端提前准备好了(这点最简单，下面的代码就可以用)，也就是msg->buff_len !=
// 0，一定不等于0
// - 所以要求两端的用户 都必须提前申请好自己的buf和buf_len
int mr_create(create_qp_res_t *res, rdma_trans_wr_t *msg) {

  fprintf(stderr, "you are in mr_create() \n");
  int mr_flags = 0;
  size_t size;
  int rc = 0, i = 0;

  /* allocate the memory buffer that will hold the data */
  if (msg->wr_len != 0) {
    size = msg->wr_len;
  } else {

    if (res->remote_props == NULL || res->remote_props->mrs.len == 0) {
      fprintf(stderr, "failed to create mr3\n");
      // rdma_trans_exit(res,msg,config);
      rc = 1;
      return rc;
    } else {
      size = res->remote_props->mrs.len;
    }
  }

  fprintf(stderr, "mr_create  msg->wr_len: %ld \n", msg->wr_len);
  fprintf(stderr, "mr_create  size: %ld \n", size);

  if (!msg->sges || !msg->sges[0].buf) {
    fprintf(stderr, "mr_create  server should be here 1 \n");
    // msg->wr_len = size;
    msg->sge_count = 1;
    msg->sges =
        (rdma_trans_sge_t *)malloc(sizeof(rdma_trans_sge_t) * msg->sge_count);
    for (i = 0; i < msg->sge_count; ++i) {
      fprintf(stderr, "mr_create  server should be here 2 \n");
      msg->sges[i].buf =
          (char *)malloc(size); // - 这地方以后 malloc时候需要注意 -
      msg->sges[i].buf_len = size;
      if (!msg->sges[i].buf) {
        fprintf(stderr, "mr_create  ->  malloc buff failed \n");
        rc = 1;
        return rc;
      } else {
        memset(msg->sges[i].buf, 0, size);
      }
    }
  }

  /* register the memory buffer */
  mr_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

  for (i = 0; i < msg->sge_count; ++i) {
    msg->sges[i].mr =
        ibv_reg_mr(res->pd, msg->sges[i].buf, msg->sges[i].buf_len, mr_flags);

    if (!msg->sges[i].mr) {
      fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
      return 1;
    }
  }

  /* - 注册rdma_cdc的mr (仅当尚未注册时) - */
  if (!res->cdc_mr) {
    rdma_cdc_t *cdc = (rdma_cdc_t *)malloc(sizeof(rdma_cdc_t));
    memset(cdc, 0, sizeof(rdma_cdc_t));
    res->cdc_mr = ibv_reg_mr(res->pd, cdc, sizeof(rdma_cdc_t),
                             IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_WRITE);
    if (!res->cdc_mr) {
      fprintf(stderr, "CDC: Failed to register MR for CDC message\n");
      free(cdc);
      return 1;
    }
  }

  return rc;
}

void mr_destroy(rdma_trans_wr_t *msg) {

  int i = 0;
  if (msg != NULL && msg->sges) {

    for (i = 0; i < msg->sge_count; ++i) {
      if (ibv_dereg_mr(msg->sges[i].mr)) {
        fprintf(stderr, "failed to deregister MR\n");
      }
      msg->sges[i].mr = NULL;
    }
  }
}

int qp_create(create_qp_res_t *res, int max_send_wr, int max_recv_wr,
              int max_send_sge, int max_recv_sge) {

  int rc = 0;

  struct ibv_qp_init_attr qp_init_attr;
  /* create the Queue Pair */
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.sq_sig_all = 0;
  /* 关键修正：禁用全量信号，允许 post_send_cdc 使用 unsignaled send */
  qp_init_attr.send_cq = res->cq;
  qp_init_attr.recv_cq = res->cq;
  qp_init_attr.cap.max_send_wr = max_send_wr;
  qp_init_attr.cap.max_recv_wr = max_recv_wr;
  qp_init_attr.cap.max_send_sge = max_send_sge;
  qp_init_attr.cap.max_recv_sge = max_recv_sge;
  res->qp = ibv_create_qp(res->pd, &qp_init_attr);
  if (!res->qp) {
    fprintf(stderr, "failed to create QP\n");
    qp_destroy(res);
    rc = 1;
    return rc;
  }

  gettimeofday(&res->start_time, NULL);
  res->is_alive = true;

  return rc;
}

void qp_destroy(create_qp_res_t *res) {

  if (res->qp) {
    if (ibv_destroy_qp(res->qp)) {
      fprintf(stderr, "failed to destroy QP\n");
    }
    res->qp = NULL;
  }
  res->is_alive = false;
  res->start_time.tv_sec = 0;
  res->start_time.tv_usec = 0;
}

int modify_qp_to_init(struct ibv_qp *qp, config_t *config) {

  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = config->ib_port;
  attr.pkey_index = 0;
  attr.qp_access_flags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) {
    fprintf(stderr, "failed to modify QP state to INIT\n");
  }
  return rc;
}

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn,
                            uint16_t dlid, uint8_t *dgid, config_t *config) {
  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));

  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu =
      IBV_MTU_256; // -
                   // 还有IBV_MTU_256、IBV_MTU_512、IBV_MTU_1024、IBV_MTU_2048、IBV_MTU_4096
                   // -
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 0x12;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = dlid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = config->ib_port;
  if (config->gid_idx >= 0) {
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = 1;
    memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = config->gid_idx;
    attr.ah_attr.grh.traffic_class = 0;
  }

  flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
          IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) {
    fprintf(stderr, "failed to modify QP state to RTR\n");
  }
  return rc;
}

static int modify_qp_to_rts(struct ibv_qp *qp) {
  struct ibv_qp_attr attr;
  int flags;
  int rc = 0;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 0x12;
  attr.retry_cnt = 6;
  attr.rnr_retry = 0;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
          IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
  rc = ibv_modify_qp(qp, &attr, flags);
  if (rc) {
    fprintf(stderr, "failed to modify QP state to RTS\n");
  }
  return rc;
}

static int connect_qp(create_qp_res_t *res, rdma_trans_wr_t *msg,
                      config_t *config) {
  struct cm_con_data_ex_t local_con_data;
  struct cm_con_data_ex_t remote_con_data;
  struct cm_con_data_ex_t tmp_con_data;
  int rc = 0;
  union ibv_gid my_gid;
  if (config->gid_idx >= 0) {
    rc = ibv_query_gid(res->ib_ctx, config->ib_port, config->gid_idx, &my_gid);
    if (rc) {
      fprintf(stderr, "could not get gid for port %d, index %d\n",
              config->ib_port, config->gid_idx);
      return rc;
    }
  } else {
    memset(&my_gid, 0, sizeof my_gid);
  }

  /* exchange using TCP sockets info required to connect QPs */
  local_con_data.mrs.addr = htonll((uintptr_t)msg->sges[0].buf);
  local_con_data.mrs.rkey = htonl(msg->sges[0].mr->rkey);
  local_con_data.mrs.len = htonll(msg->wr_len);
  local_con_data.qp_num = htonl(res->qp->qp_num);

  local_con_data.lid = htons(res->port_attr.lid);
  memcpy(local_con_data.gid, &my_gid, 16);

  if (sock_sync_data(res->sock, sizeof(struct cm_con_data_ex_t),
                     (char *)&local_con_data, (char *)&tmp_con_data) < 0) {
    fprintf(stderr, "failed to exchange connection data between sides\n");
    rc = 1;
    return rc;
  }

  remote_con_data.mrs.addr = ntohll(tmp_con_data.mrs.addr);
  remote_con_data.mrs.rkey = ntohl(tmp_con_data.mrs.rkey);
  remote_con_data.mrs.len = ntohll(tmp_con_data.mrs.len);
  remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
  remote_con_data.lid = ntohl(tmp_con_data.lid);
  // remote_con_data.mr_count = ntohl(tmp_con_data.mr_count);
  memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

  /* save the remote side attributes, we will need it for the post SR */
  // res->remote_props = &remote_con_data;
  res->remote_props =
      (struct cm_con_data_ex_t *)malloc(sizeof(struct cm_con_data_ex_t));
  memcpy(res->remote_props, &remote_con_data, sizeof(struct cm_con_data_ex_t));

  if (config->gid_idx >= 0) {
    uint8_t *p = remote_con_data.gid;
  }

  struct ibv_qp_attr query_attr;
  struct ibv_qp_init_attr init_attr;
  rc = ibv_query_qp(res->qp, &query_attr, IBV_QP_STATE, &init_attr);
  if (rc == 0 && query_attr.qp_state == IBV_QPS_RESET) {

    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp, config);
    if (rc) {
      fprintf(stderr, "change QP state to INIT failed\n");
      return rc;
    }

    /* - 根据 sync_mode post 对应的 receive - */
    if (config->sync_mode == RDMA_SYNC_CDC) {
      rc = post_receive_cdc(res->cdc_mr->addr, res->qp, res->cdc_mr);
      if (rc) {
        fprintf(stderr, "in connect qp: post_receive_cdc failed\n");
        return rc;
      }
    }
    /* RDMA_SYNC_NONE: 不调用任何 post_receive */

    /* modify the QP to RTR */
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid,
                          remote_con_data.gid, config);
    if (rc) {
      fprintf(stderr, "failed to modify QP state to RTR\n");
      return rc;
    }

    /* modify the QP to RTS */
    rc = modify_qp_to_rts(res->qp);
    if (rc) {
      fprintf(stderr, "failed to modify QP state to RTS\n");
      return rc;
    }
  }

  return rc;
}

void rdma_trans_destroy(create_qp_res_t *res, rdma_trans_wr_t *msg,
                        config_t *config) {

  int i = 0;

  if (res->qp) {
    if (ibv_destroy_qp(res->qp)) {
      fprintf(stderr, "failed to destroy QP\n");
      // rc = 1;
    }
  }

  if (msg != NULL && msg->sges) {
    for (i = 0; i < msg->sge_count; ++i) {
      if (ibv_dereg_mr(msg->sges[i].mr)) {
        fprintf(stderr, "failed to deregister MR\n");
      }
      msg->sges[i].mr = NULL;
    }
  }
  if (res->cq) {
    if (ibv_destroy_cq(res->cq)) {
      fprintf(stderr, "failed to destroy CQ\n");
      // rc = 1;
    }
  }

  if (res->pd) {
    if (ibv_dealloc_pd(res->pd)) {
      fprintf(stderr, "failed to deallocate PD\n");
      // rc = 1;
    }
  }

  if (res->ib_ctx) {
    if (ibv_close_device(res->ib_ctx)) {
      fprintf(stderr, "failed to close device context\n");
      // rc = 1;
    }
  }

  if (res->sock >= 0) {
    if (close(res->sock)) {
      fprintf(stderr, "failed to close socket\n");
      // rc = 1;
    }
  }

  if (res->remote_props) {
    free(res->remote_props);
    res->remote_props = NULL;
  }

  if (res->cdc_mr) {
    void *cdc_buffer = (void *)(uintptr_t)res->cdc_mr->addr;
    ibv_dereg_mr(res->cdc_mr);
    res->cdc_mr = NULL;
    if (cdc_buffer) {
      free(cdc_buffer);
    }
  }

  if (config) {
    if (config->dev_name) {
      free((char *)config->dev_name);
      config->dev_name = NULL;
    }

    if (config->server_name) {
      free((char *)config->server_name);
      config->server_name = NULL;
    }
  }
}

int rdma_context_init(create_qp_res_t *res, config_t *config) {

  int rc = 0;

  struct ibv_device **dev_list = NULL;
  struct ibv_device *ib_dev = NULL;
  int i = 0;
  int num_devices;

  resources_init(res);
  /* get device names in the system */
  dev_list = ibv_get_device_list(&num_devices);
  if (!dev_list) {
    fprintf(stderr, "failed to get IB devices list\n");
    rc = 1;
    context_res_exit(res, rc, dev_list);
    return rc;
  }

  /* if there isn't any IB device in host */
  if (!num_devices) {
    fprintf(stderr, "found %d device(s)\n", num_devices);
    rc = 1;
    context_res_exit(res, rc, dev_list);
    return rc;
  }
  fprintf(stdout, "found %d device(s)\n", num_devices);

  /* search for the specific device we want to work with */
  for (i = 0; i < num_devices; i++) {
    if (!config->dev_name) {
      config->dev_name = strdup(ibv_get_device_name(dev_list[i]));
      fprintf(stderr, "device not specified, using first one found: %s\n",
              config->dev_name);
    }
    /* find the specific device */
    if (!strcmp(ibv_get_device_name(dev_list[i]), config->dev_name)) {
      ib_dev = dev_list[i];
      break;
    }
  }

  /* if the device wasn't found in host */
  if (!ib_dev) {
    fprintf(stderr, "IB device %s wasn't found\n", config->dev_name);
    rc = 1;
    context_res_exit(res, rc, dev_list);
    return rc;
  }

  /* get device handle */
  res->ib_ctx = ibv_open_device(ib_dev);
  if (!res->ib_ctx) {
    fprintf(stderr, "failed to open device %s\n", config->dev_name);
    rc = 1;
    context_res_exit(res, rc, dev_list);
    return rc;
  }

  /* We are now done with device list, free it */
  ibv_free_device_list(dev_list);
  dev_list = NULL;
  ib_dev = NULL;

  /* query port properties */
  if (ibv_query_port(res->ib_ctx, config->ib_port, &res->port_attr)) {
    fprintf(stderr, "ibv_query_port on port %u failed\n", config->ib_port);
    rc = 1;
    context_res_exit(res, rc, dev_list);
    return rc;
  }

  return rc;
}

/*
-   下面开始准备发送了，注册mr也是准备发送
    代表之前的qp什么的，交换建链信息的都已经准备好了，就要开始传输信息了 -


*/

int rdma_trans_init(create_qp_res_t *res, rdma_trans_wr_t *msg,
                    config_t *config) {
  int rc = 0;

  if (mr_create(res, msg)) { // - 注册mr的时候 强制的让他们都在同一个pd下 -
    fprintf(stderr, "mr_create failed register mr  \n");
    rc = 1;
  }

  if (connect_qp(res, msg, config)) {
    fprintf(stderr, "failed to connect QPs\n");
    // rdma_trans_exit(res,msg,config);
    rc = 1;
    // return rc;
  }

  if (rc) {
    rdma_trans_destroy(res, msg, config);
  }

  return rc;
}

int rdma_trans_post(create_qp_res_t *res, rdma_trans_wr_t *msg,
                    config_t *config) {
  int rc = 0;

  if (post_send(res, msg)) {
    fprintf(stderr, "failed to post SR 0 \n");
    rc = 1;
    rdma_trans_destroy(res, msg, config);

    return rc;
  }

  rc = 0;
  return rc;
}

int rdma_trans_completion(create_qp_res_t *res) {

  int rc = 0;

  if (poll_completion(res)) {
    fprintf(stderr, "poll completion failed 2\n");
    rc = 1;
  }
  return rc;
}
