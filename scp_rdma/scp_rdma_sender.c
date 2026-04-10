/*
 * scp_rdma_sender.c - SCP over RDMA 发送端实现 (双缓冲优化版)
 */

#define _GNU_SOURCE /* O_DIRECT 需要此宏才能在 <fcntl.h> 中定�?*/

#include "scp_rdma.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * 收割已完成的 ACK，更�?slot 状态为空闲
 * 使用 rdma_async_wait_completion 进行事件驱动收割（非忙轮询）
 * @param blocking 是否阻塞等待（true=等待事件，false=非阻塞检查）
 * @return 收割�?WC 数量�?1 表示错误
 */
static void handle_ack_recv(scp_rdma_context_t *ctx, uint64_t wr_id) {
  rdma_cdc_t *cdc = rdma_cdc_consume_buf(&ctx->res);
  int slot_id = -1;
  if (cdc) {
    slot_id = (int)cdc->slot_id;
  } else {
    slot_id = (int)wr_id;
  }

  if (slot_id >= 0 && slot_id < DUAL_BUFFER_SLOTS) {
    fprintf(stderr, "[SENDER DEBUG] ACK received slot=%d pending_before=%d\n",
            slot_id, ctx->pending_acks);
    if (!ctx->slot_free[slot_id]) {
      ctx->slot_free[slot_id] = true;
      ctx->pending_acks--;
    } else {
      fprintf(stderr, "[SENDER DEBUG] ACK slot=%d already free (dup)\n",
              slot_id);
    }
    fprintf(stderr, "[SENDER DEBUG] slot=%d freed pending_after=%d\n", slot_id,
            ctx->pending_acks);
  } else {
    fprintf(stderr,
            "[SENDER DEBUG] ACK with invalid slot_id=%d (wr_id=%lu)\n",
            slot_id, wr_id);
  }
}

static int harvest_completions(scp_rdma_context_t *ctx, bool blocking) {
  struct ibv_wc wc[16];
  int timeout_ms = blocking ? 100 : 0; /* 阻塞时最多等�?100ms */

  int ne = rdma_async_wait_completion(&ctx->res, wc, 16, timeout_ms);
  if (ne < 0) {
    fprintf(stderr, "async-pipeline: completion error\n");
    return -1;
  }

  int harvested = 0;
  int errors = 0; /* 错误计数 */

  for (int i = 0; i < ne; i++) {
    if (wc[i].status != IBV_WC_SUCCESS) {
      fprintf(stderr,
              "async-pipeline: WC error status=0x%x, opcode=%d, wr_id=%lu\n",
              wc[i].status, wc[i].opcode, wc[i].wr_id);
      errors++;
      continue;
    }

    switch (wc[i].opcode) {
    case IBV_WC_RDMA_WRITE:
      /* Write 完成 - 只要能到这里说明成功，SQE 已释�?*/
      /* 不释�?slot，需要等 ACK */
      break;

    case IBV_WC_RECV:
      handle_ack_recv(ctx, wc[i].wr_id);
      harvested++;
      break;

    case IBV_WC_SEND:
      /* CDC Send 完成，SQE 已释�?*/
      break;

    default:
      break;
    }
  }

  /* 如果有错误，返回 -1 终止传输 */
  if (errors > 0) {
    fprintf(stderr, "async-pipeline: %d WC errors detected, aborting\n",
            errors);
    return -1;
  }

  return harvested;
}
/* �ȴ� RDMA WRITE ��ɣ�ͬʱ���� ACK������ͬ�� poll_completion ���� RECV */
static int wait_for_rdma_write_completion(scp_rdma_context_t *ctx) {
  struct ibv_wc wc[16];

  while (1) {
    int ne = rdma_async_wait_completion(&ctx->res, wc, 16, 100);
    if (ne < 0) {
      fprintf(stderr, "sync-write: completion error\n");
      return -1;
    }
    if (ne == 0) {
      continue;
    }

    bool write_done = false;
    for (int i = 0; i < ne; i++) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        fprintf(stderr,
                "sync-write: WC error status=0x%x, opcode=%d, wr_id=%lu\n",
                wc[i].status, wc[i].opcode, wc[i].wr_id);
        return -1;
      }

      switch (wc[i].opcode) {  
      case IBV_WC_RDMA_WRITE: // - 发送端在传输完成后收的的cqe，里面的opcode是IBV_WC_RDMA_WRITE - 
        write_done = true; // - 就可以继续告诉接收端信息了，也就是 post SEND操作 - 
        break;
      case IBV_WC_RECV: // - sender端post RECEIVE操作后，接收到接收端发来的SEND操作产生的cqe - 
        handle_ack_recv(ctx, wc[i].wr_id);
        break;
      case IBV_WC_SEND: // - sender端post SEND操作后，自己收到的cqe - 
        break;
      default:
        break;
      }
    }

    if (write_done) {
      return 0;
    }
  }
}

/**
 * 等待指定 slot 变为空闲（阻塞收�?ACK�? */
static int wait_for_slot(scp_rdma_context_t *ctx, int slot_id) {
  while (!ctx->slot_free[slot_id]) {
    if (harvest_completions(ctx, true) < 0) {
      return -1;
    }
  }
  return 0;
}

/**
 * 等待所有待确认�?ACK
 */
static int drain_pending_acks(scp_rdma_context_t *ctx) {
  while (ctx->pending_acks > 0) {
    if (harvest_completions(ctx, true) < 0) {
      return -1;
    }
  }
  return 0;
}

/* ===================== 原有同步发送函�?===================== */

/* 内部辅助函数：发送一个RDMA请求并等待同�?(Prepare ACK -> Write Data -> Signal
 * -> Wait ACK) */
static int scp_rdma_send_sync(scp_rdma_context_t *ctx, rdma_trans_wr_t *wr) {
  /* 0. 确保 slot 0 空闲（控制报文固定走 slot0�?*/
  if (wait_for_slot(ctx, 0) < 0) {
    return -1;
  }

  /* 1. 先准备好接收 ACK 的“口袋”（统一 wr_id=0�?*/
  rdma_cdc_t *ack_buf = rdma_cdc_acquire_post_buf(&ctx->res);
  if (!ack_buf ||
      post_receive_cdc_ex(ack_buf, ctx->res.qp, ctx->res.cdc_mr, 0)) {
    fprintf(stderr, "Failed to post ACK receive\n");
    return -1;
  }
  fprintf(stderr, "[SENDER DEBUG] sync: post_recv_ack slot=0\n");

  /* 2. RDMA Write (数据/元数�? */
  if (rdma_trans_post(&ctx->res, wr, &ctx->rdma_config)) {
    fprintf(stderr, "Failed to post RDMA WR\n");
    return -1;
  }

  if (wait_for_rdma_write_completion(ctx)) {
    fprintf(stderr, "Failed to get RDMA completion\n");
    return -1;
  }

  /* 3. 发�?Data Ready 信号 */
  /* 控制报文固定�?slot0，统一�?harvest_completions 处理 ACK */
  ctx->slot_free[0] = false;
  ctx->pending_acks++;
  rdma_cdc_t *cdc = ctx->res.cdc_send_buf;
  memset(cdc, 0, sizeof(*cdc));
  cdc->slot_id = 0;     /* 同步发送总是使用 slot 0 */
  cdc->signal_type = 0; /* DATA_READY */
  cdc->client_is_write = 1;

  fprintf(stderr, "[SENDER DEBUG] send sync DATA_READY slot=0\n");
  if (post_send_cdc(ctx->res.cdc_send_buf, ctx->res.qp, ctx->res.cdc_mr)) {
    fprintf(stderr, "Failed to post CDC sync\n");
    ctx->slot_free[0] = true;
    ctx->pending_acks--;
    return -1;
  }

  /* 4. 等待接收端的 ACK 信号 */
  if (wait_for_slot(ctx, 0) < 0) {
    return -1;
  }
  fprintf(stderr, "[SENDER DEBUG] sync: ACK received slot=0\n");

  return 0;
}

int scp_rdma_init(scp_rdma_context_t *ctx, const scp_rdma_config_t *cfg) {
  memset(ctx, 0, sizeof(scp_rdma_context_t));
  ctx->config = *cfg;

  /* 映射配置到底层库格式 */
  ctx->rdma_config.dev_name = cfg->dev_name;
  ctx->rdma_config.server_name = cfg->server_addr;
  ctx->rdma_config.tcp_port = cfg->port;
  ctx->rdma_config.ib_port = cfg->ib_port;
  ctx->rdma_config.gid_idx = cfg->gid_idx;
  ctx->rdma_config.sync_mode = RDMA_SYNC_CDC; /* 统一使用 CDC */
  ctx->rdma_config.use_rdma_cm = 0;           /* 统一使用 socket */

  /* 初始化基础 RDMA 上下�?*/
  if (rdma_context_init(&ctx->res, &ctx->rdma_config))
    return -1;
  if (pd_create(&ctx->res))
    return -1;
  /* 使用异步 CQ 支持事件驱动完成通知 */
  if (cq_create_async(&ctx->res, CQ_MAX_SIZE))
    return -1;
  if (qp_create(&ctx->res, QP_MAX_SEND_WR, QP_MAX_RECV_WR, QP_MAX_SEND_SGE,
                QP_MAX_RECV_SGE))
    return -1;

  /* 预分配持久会话缓冲区 - 使用 2x 大小支持双缓�?*/
  // ctx->session_buf = malloc(SCP_DUAL_BUFFER_TOTAL_SIZE);
  // if (!ctx->session_buf)
  //   return -1;

  /* ========== 显式 4KB 页对齐分�?(针对 4-slot 流水线缓冲区) ========== */
  if (posix_memalign(&ctx->session_buf, 4096, SCP_MULTI_BUFFER_TOTAL_SIZE) !=
      0) {
    fprintf(stderr, "Failed to allocate aligned session buffer\n");
    return -1;
  }
  memset(ctx->session_buf, 0, SCP_MULTI_BUFFER_TOTAL_SIZE);

  ctx->session_sge = (rdma_trans_sge_t *)malloc(sizeof(rdma_trans_sge_t));
  memset(ctx->session_sge, 0, sizeof(rdma_trans_sge_t));
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len =
      SCP_MULTI_BUFFER_TOTAL_SIZE; /* 必须设置，供 mr_create 使用 */

  /* ========== 四缓冲初始化（偏移量方式�?========== */
  ctx->dual_buffer_enabled = true;
  ctx->pending_acks = 0;
  ctx->current_slot = 0;

  /* slot 指针指向 session_buf 的四个区�?*/
  for (int i = 0; i < DUAL_BUFFER_SLOTS; i++) {
    ctx->slot_buf[i] = (char *)ctx->session_buf + i * SCP_COMBINED_CHUNK_SIZE;
    ctx->slot_free[i] = true;
    ctx->slot_mr[i] = NULL; /* 共用 session_mr，不单独注册 */
  }

  ctx->state = SCP_STATE_READY;
  ctx->session_established = false;
  return 0;
}

static int scp_prepare_transfer_resources(scp_rdma_context_t *ctx,
                                          rdma_trans_wr_t *wr, size_t len) {
  /* 关键修正：使用四缓冲总大小进行注册和握手 */
  /* MR 必须覆盖四个 slot，每�?slot 可容纳一个完整的数据�?*/
  wr->wr_len = SCP_MULTI_BUFFER_TOTAL_SIZE;

  /* sock_established 会交换长度并触发对端分配内存 */
  if (sock_established(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  /* rdma_trans_init 负责注册 MR 和连�?QP */
  if (rdma_trans_init(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  return 0;
}

/* 内部辅助：清理传输资�?- 已废弃，持久模式下统一销�?*/

/**
 * 发送符号链�? */
static int scp_send_symlink(scp_rdma_context_t *ctx, const char *local_path,
                            const char *remote_path, const struct stat *st) {
  char link_target[SCP_MAX_PATH_LEN];
  ssize_t len = readlink(local_path, link_target, sizeof(link_target) - 1);
  if (len < 0) {
    perror("readlink");
    return -1;
  }
  link_target[len] = '\0';

  /* 1. 发送链接元数据 */
  scp_file_meta_t *meta = (scp_file_meta_t *)ctx->session_buf;
  memset(meta, 0, sizeof(scp_file_meta_t));
  scp_init_packet_header(&meta->header, SCP_CMD_FILE_META,
                         sizeof(scp_file_meta_t) - sizeof(scp_packet_header_t));

  strncpy(meta->file_name, remote_path, SCP_MAX_FILENAME_LEN - 1);
  meta->file_size = len; /* 对于链接，大小为目标路径长度 */
  meta->file_mode = st->st_mode;
  meta->file_type = SCP_FILE_SYMLINK;
  meta->total_chunks = 1;

  rdma_trans_wr_t wr_meta = {.op_type = RDMA_OP_WRITE,
                             .sges = ctx->session_sge,
                             .sge_count = 1,
                             .wr_len = sizeof(scp_file_meta_t)};

  if (!ctx->session_established) {
    if (scp_prepare_transfer_resources(ctx, &wr_meta, wr_meta.wr_len)) {
      return -1;
    }
    ctx->session_mr = ctx->session_sge->mr;
    ctx->session_established = true;
  }

  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len = sizeof(scp_file_meta_t);
  if (scp_rdma_send_sync(ctx, &wr_meta)) {
    return -1;
  }

  /* 2. 发送链接目标路径作为数据内�?(使用同步方式) */
  /* 复用 slot 0，确�?slot 0 此时是空闲的 (scp_rdma_send_sync 会保证这一�? */
  int slot = 0;
  char *slot_base = (char *)ctx->slot_buf[slot];
  scp_data_chunk_t *data_header = (scp_data_chunk_t *)slot_base;
  char *data_ptr = slot_base + sizeof(scp_data_chunk_t);

  memset(data_header, 0, sizeof(scp_data_chunk_t));
  scp_init_packet_header(&data_header->header, SCP_CMD_FILE_DATA,
                         len + sizeof(scp_data_chunk_t) -
                             sizeof(scp_packet_header_t));
  data_header->chunk_index = 0;
  data_header->chunk_size = (uint32_t)len;
  data_header->offset = 0;

  memcpy(data_ptr, link_target, len);

  rdma_trans_wr_t wr_data = {.op_type = RDMA_OP_WRITE,
                             .sges = ctx->session_sge,
                             .sge_count = 1,
                             .wr_len = sizeof(scp_data_chunk_t) + len,
                             .remote_offset =
                                 (uint64_t)slot * SCP_COMBINED_CHUNK_SIZE};

  ctx->session_sge->buf = slot_base;
  ctx->session_sge->buf_len = wr_data.wr_len;

  if (scp_rdma_send_sync(ctx, &wr_data)) {
    return -1;
  }

  ctx->total_symlinks++;
  return 0;
}

/* -功能�?  使用双缓冲流水线发送一个完整的文件(一个完整的文件包含文件元数据和文件数据�?
  -参数�?    - ctx: scp_rdma_context_t
  结构体指针，管理整个程序的生命周期，维持所有运行时状态和资源
    - local_path: 本地文件路径
    - remote_path: 远程文件路径
  -返回值：
    - 0: 成功
    - -1: 失败
*/
int scp_send_file(scp_rdma_context_t *ctx, const char *local_path,
                  const char *remote_path) {

  // - 准备文件元数�?-
  struct stat st;
  if (stat(local_path, &st)) { // - 获取文件大小以及权限 -
    perror("stat");
    return -1;
  }
  /* ========== O_DIRECT 零拷�?DMA：绕�?Page Cache ========== */
  /* O_DIRECT �?read/pread �?DMA 路径，磁盘数据直接到用户�?MR 缓冲区，
   * 消除 CPU 拷贝。如果文件系统不支持 O_DIRECT 则自动回退到普�?read�?*/
  bool use_direct_io = false;
  int fd = open(local_path, O_RDONLY | O_DIRECT);
  if (fd >= 0) {
    use_direct_io = true;
  } else {
    /* O_DIRECT 打开失败（如 tmpfs 不支持），回退到普通模�?*/
    fd = open(local_path, O_RDONLY);
    if (fd < 0) {
      perror("open");
      return -1;
    }
  }

  // - 下面开始构建meta�?-

  /* 1. 发送文件元数据 */
  scp_file_meta_t *meta =
      (scp_file_meta_t *)
          ctx->session_buf; /* - [内存复用] 直接使用 session_buf�?KB header +
                               16MB data�?的头部作�?meta 包的内存 */
  memset(meta, 0, sizeof(scp_file_meta_t));
  scp_init_packet_header(
      &meta->header, SCP_CMD_FILE_META,
      sizeof(scp_file_meta_t) -
          sizeof(scp_packet_header_t)); /* - 初始化协议头，标记为META - */

  strncpy(meta->file_name, remote_path, SCP_MAX_FILENAME_LEN - 1);
  meta->file_size = st.st_size;
  meta->file_mode = st.st_mode;
  meta->file_type = SCP_FILE_REGULAR;
  meta->total_chunks = (st.st_size + SCP_CHUNK_SIZE - 1) / SCP_CHUNK_SIZE;

  /* 注意：在持久模式下，session_sge->buf_len �?scp_rdma_init 中已设为 1MB */
  /* 我们通过 wr_meta.wr_len 控制初次握手的交换长�?*/
  rdma_trans_wr_t wr_meta = {
      .op_type = RDMA_OP_WRITE,
      .sges = ctx->session_sge,
      .sge_count = 1,
      .wr_len = sizeof(scp_file_meta_t)}; /* - 构�?RDMA 写请求，长度仅为 meta
                                             结构体大�?- */

  /* 建立会话或复用连�?*/
  if (!ctx->session_established) {
    /* 这里 wr_meta.wr_len 已在 scp_prepare_transfer_resources 中被强制设为
     * SCP_COMBINED_CHUNK_SIZE */
    if (scp_prepare_transfer_resources(ctx, &wr_meta, wr_meta.wr_len)) {
      close(fd);
      return -1;
    }
    /* 握手完成后，捕获被注册的 MR 以便后续复用 */
    ctx->session_mr = ctx->session_sge->mr;
    ctx->session_established = true;

    /* 注意：以前这里需要手�?post_receive_cdc�?     * 现在已移�?scp_rdma_send_sync 内部自动处理，更加鲁�?*/
  }

  /* 在实际发送前，重�?SGE 指向 session_buf 并设置长度�?   * 虽然 MR �?1MB 的，但我们只需要发�?sizeof(scp_file_meta_t)
   * 注意：如果之前发送过文件数据，session_sge->buf 可能指向某个 slot */
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len = sizeof(scp_file_meta_t);
  if (scp_rdma_send_sync(ctx, &wr_meta)) { /* - 同步发�?meta �?- */
    close(fd);
    return -1;
  }

  /* 2. 分块发送文件数�?- 双缓冲流水线模式 */
  uint32_t total_chunks = meta->total_chunks;
  uint64_t file_size = meta->file_size;
  uint64_t offset = 0;

  /* 初始化四缓冲状�?*/
  ctx->current_slot = 0;
  ctx->pending_acks = 0;
  for (int s = 0; s < DUAL_BUFFER_SLOTS; s++) {
    ctx->slot_free[s] = true;
  }

  for (uint32_t i = 0; i < total_chunks; i++) {
    int slot = ctx->current_slot;

    /* 1. 等待当前 slot 变为空闲 */
    if (wait_for_slot(ctx, slot) < 0) {
      fprintf(stderr, "dual-buffer: wait_for_slot failed for slot %d\n", slot);
      close(fd);
      return -1;
    }

    /* 2. 读取数据到当�?slot 的缓冲区（零拷贝 DMA 路径�?*/
    char *slot_base = (char *)ctx->slot_buf[slot];
    /* 数据区从 slot_base + SCP_DATA_OFFSET (4KB 对齐) 开始，
     * 满足 O_DIRECT 对缓冲区地址对齐的要�?*/
    char *data_ptr = slot_base + SCP_DATA_OFFSET;

    /* 计算本次读取大小 */
    size_t to_read = SCP_CHUNK_SIZE;
    if (offset + to_read > file_size)
      to_read = (size_t)(file_size - offset);

    ssize_t n;
    if (use_direct_io) {
      /* O_DIRECT 要求读取大小为块大小倍数，向上取整到 4KB */
      size_t aligned_size = (to_read + 4095) & ~(size_t)4095;
      n = pread(fd, data_ptr, aligned_size, (off_t)offset);
      /* pread 返回实际读到的字节数（可能小�?aligned_size�?*/
      if (n > (ssize_t)to_read)
        n = (ssize_t)to_read; /* 裁剪到实际文件剩余大�?*/
    } else {
      /* 普通模式回退 */
      n = pread(fd, data_ptr, to_read, (off_t)offset);
    }
    if (n <= 0)
      break;

    /* 3. 填充数据�?*/
    scp_data_chunk_t *data_header = (scp_data_chunk_t *)slot_base;
    memset(data_header, 0, sizeof(scp_data_chunk_t));
    scp_init_packet_header(&data_header->header, SCP_CMD_FILE_DATA,
                           n + sizeof(scp_data_chunk_t) -
                               sizeof(scp_packet_header_t));
    data_header->chunk_index = i;
    data_header->chunk_size = (uint32_t)n;
    data_header->offset = offset;

    /* 4. 预发�?ACK 接收（使�?slot 作为 wr_id 标识�?*/
    fprintf(stderr, "[SENDER DEBUG] post_recv_ack slot=%d\n", slot);
    rdma_cdc_t *ack_buf = rdma_cdc_acquire_post_buf(&ctx->res);
    if (!ack_buf ||
        post_receive_cdc_ex(ack_buf, ctx->res.qp, ctx->res.cdc_mr,
                            (uint64_t)slot)) {
      fprintf(stderr, "dual-buffer: failed to post ACK receive for slot %d\n",
              slot);
      close(fd);
      return -1;
    }

    /* 5. 使用 2-SGE scatter/gather 发送：header + data（跳过中�?pad�?*/
    /* SGE[0] = header (slot_base 起始，CPU 填写的元数据)
     * SGE[1] = data   (slot_base + SCP_DATA_OFFSET，O_DIRECT DMA 的文件数�?
     * RDMA NIC 自动聚合两段数据，写到远端时 [header|data] 紧密排列，无 pad */
    rdma_trans_sge_t data_sges[2];

    /* SGE[0]: 数据块头 */
    data_sges[0].buf = slot_base;
    data_sges[0].buf_len = sizeof(scp_data_chunk_t);
    data_sges[0].mr = ctx->session_mr;

    /* SGE[1]: 文件数据�?KB 对齐区域，O_DIRECT DMA 目标�?*/
    data_sges[1].buf = data_ptr;
    data_sges[1].buf_len = (size_t)n;
    data_sges[1].mr = ctx->session_mr;

    /* 设置远程地址偏移，确保写入正确的远程 slot */
    uint64_t remote_slot_offset = (uint64_t)slot * SCP_COMBINED_CHUNK_SIZE;

    rdma_trans_wr_t wr_data = {.op_type = RDMA_OP_WRITE,
                               .sges = data_sges,
                               .sge_count = 2,
                               .wr_len = sizeof(scp_data_chunk_t) + n,
                               .remote_offset = remote_slot_offset};

    if (rdma_trans_post(&ctx->res, &wr_data, &ctx->rdma_config)) {
      fprintf(stderr, "dual-buffer: failed to post RDMA WR slot %d\n", slot);
      close(fd);
      return -1;
    }

    /* 异步模式：不等待 RDMA Write 完成，信�?IB 顺序规则 */
    /* DEBUG: 打印发送的 chunk 信息，包括远程偏�?*/
    fprintf(stderr,
            "[SENDER DEBUG] chunk %u: slot=%d, local=%p, remote_offset=%llu, "
            "size=%zd\n",
            i, slot, (void *)slot_base, (unsigned long long)remote_slot_offset,
            n);

    /* 6. 发�?CDC 信号 (�?slot_id) */
    rdma_cdc_t *cdc = ctx->res.cdc_send_buf;
    memset(cdc, 0, sizeof(*cdc));
    cdc->slot_id = slot;
    cdc->signal_type = 0; /* DATA_READY */
    cdc->client_is_write = 1;

    fprintf(stderr, "[SENDER DEBUG] send DATA_READY slot=%d\n", slot);
    if (post_send_cdc(ctx->res.cdc_send_buf, ctx->res.qp, ctx->res.cdc_mr)) {
      fprintf(stderr, "dual-buffer: failed to post CDC slot %d\n", slot);
      close(fd);
      return -1;
    }

    /* 7. 标记 slot 为忙，切换到下一�?slot */
    ctx->slot_free[slot] = false;
    ctx->pending_acks++;
    ctx->current_slot = (slot + 1) % DUAL_BUFFER_SLOTS; /* 0->1->2->3->0 轮转 */

    offset += n;
    ctx->total_bytes += n;
    if (ctx->config.progress_cb) {
      ctx->config.progress_cb(local_path, offset, st.st_size);
    }

    /* 8. 尝试非阻塞收割完成事件（不等待，只收割已完成的） */
    if (harvest_completions(ctx, false) < 0) {
      fprintf(stderr, "dual-buffer: harvest_completions failed\n");
      close(fd);
      return -1;
    }
  }

  /* 发送完成后，等待所有待确认�?ACK */
  if (drain_pending_acks(ctx) < 0) {
    fprintf(stderr, "dual-buffer: drain_pending_acks failed\n");
    close(fd);
    return -1;
  }

  close(fd);
  ctx->total_files++;
  return 0;
}

int scp_send_directory(scp_rdma_context_t *ctx, const char *local_path,
                       const char *remote_path) {
  DIR *dir = opendir(local_path);
  if (!dir) {
    perror("opendir");
    return -1;
  }

  /* 1. 发送进入目录指�?*/
  scp_dir_info_t *heap_dir = (scp_dir_info_t *)ctx->session_buf;
  memset(heap_dir, 0, sizeof(scp_dir_info_t));
  scp_init_packet_header(&heap_dir->header, SCP_CMD_DIR_ENTER,
                         sizeof(scp_dir_info_t) - sizeof(scp_packet_header_t));
  strncpy(heap_dir->dir_name, remote_path, SCP_MAX_FILENAME_LEN - 1);

  /* 注意：持久模式下 session_sge->buf_len �?init 中已设为 1MB */
  rdma_trans_wr_t wr_dir = {.op_type = RDMA_OP_WRITE,
                            .sges = ctx->session_sge,
                            .sge_count = 1,
                            .wr_len = sizeof(scp_dir_info_t)};

  if (!ctx->session_established) {
    if (scp_prepare_transfer_resources(ctx, &wr_dir, wr_dir.wr_len)) {
      closedir(dir);
      return -1;
    }
    ctx->session_mr = ctx->session_sge->mr;
    ctx->session_established = true;
  }

  /* 实际发送前重置 SGE 指向 session_buf 并设置长�?*/
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len = sizeof(scp_dir_info_t);
  if (scp_rdma_send_sync(ctx, &wr_dir)) {
    fprintf(stderr, "Failed to send DIR_ENTER command\n");
    closedir(dir);
    return -1;
  }

  /* 2. 遍历目录内容 */
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char sub_local[SCP_MAX_PATH_LEN];
    char sub_remote[SCP_MAX_PATH_LEN];
    snprintf(sub_local, sizeof(sub_local), "%s/%s", local_path, entry->d_name);
    snprintf(sub_remote, sizeof(sub_remote), "%s/%s", remote_path,
             entry->d_name);

    struct stat st;
    int stat_ret;

    /* 根据配置决定是否跟随软连�?*/
    if (ctx->config.no_link) {
      stat_ret = stat(sub_local, &st); /* Follow symlinks */
    } else {
      stat_ret = lstat(sub_local, &st); /* Don't follow symlinks */
    }

    if (stat_ret)
      continue;

    if (S_ISDIR(st.st_mode)) {
      if (ctx->config.recursive) {
        /* 注意：如果是软连接指向目录且 no_link=false，stat会识别为 DIR�?         * �?lstat 会识别为 LNK。这里处理真正的目录 */
        if (scp_send_directory(ctx, sub_local, sub_remote)) {
          closedir(dir);
          return -1;
        }
      }
    } else if (S_ISLNK(st.st_mode)) {
      /* 只有 lstat 模式下才会进入此分支 */
      if (scp_send_symlink(ctx, sub_local, sub_remote, &st)) {
        closedir(dir);
        return -1;
      }
    } else {
      if (scp_send_file(ctx, sub_local, sub_remote)) {
        closedir(dir);
        return -1;
      }
    }
  }

  /* 增加目录计数 */
  ctx->total_dirs++;

  /* 3. 发送离开目录指令 */
  scp_dir_info_t *heap_leave = (scp_dir_info_t *)ctx->session_buf;
  memset(heap_leave, 0, sizeof(scp_dir_info_t));
  scp_init_packet_header(&heap_leave->header, SCP_CMD_DIR_LEAVE, 0);

  /* 关键修复：重�?SGE 指向 session_buf，因为刚刚发送过文件数据 */
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len = sizeof(scp_packet_header_t);
  rdma_trans_wr_t wr_leave = {.op_type = RDMA_OP_WRITE,
                              .sges = ctx->session_sge,
                              .sge_count = 1,
                              .wr_len = sizeof(scp_packet_header_t)};

  if (scp_rdma_send_sync(ctx, &wr_leave)) { /* - 控制命令都是同步发送的 - */
    fprintf(stderr, "Failed to send DIR_LEAVE command\n");
    closedir(dir);
    return -1;
  }

  closedir(dir);
  return 0;
}

int scp_sender_run(scp_rdma_context_t *ctx, const char *local_path,
                   const char *remote_path, scp_result_t *result) {
  (void)result; /* 标记为未使用 */
  struct stat st;
  if (stat(local_path, &st))
    return -1;

  int ret = 0;
  /* 顶层入口也需要遵循相同的 symlink 规则 */
  if (ctx->config.no_link) {
    if (stat(local_path, &st))
      return -1;
  } else {
    if (lstat(local_path, &st))
      return -1;
  }

  if (S_ISDIR(st.st_mode)) {
    ret = scp_send_directory(ctx, local_path, remote_path);
  } else if (S_ISLNK(st.st_mode)) {
    ret = scp_send_symlink(ctx, local_path, remote_path, &st);
  } else {
    ret = scp_send_file(ctx, local_path, remote_path);
  }

  if (ret) {
    fprintf(stderr, "Transfer failed, aborting\n");
    return ret;
  }

  /* 发送结束包 */
  scp_transfer_end_t *heap_end = (scp_transfer_end_t *)ctx->session_buf;
  memset(heap_end, 0, sizeof(scp_transfer_end_t));
  scp_init_packet_header(&heap_end->header, SCP_CMD_TRANSFER_END,
                         sizeof(scp_transfer_end_t) -
                             sizeof(scp_packet_header_t));

  /* 关键修复：重�?SGE 指向 session_buf，因为文件发送循环可能把它改成了 slot 1
   */
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len = sizeof(scp_transfer_end_t);
  rdma_trans_wr_t wr_end = {.op_type = RDMA_OP_WRITE,
                            .sges = ctx->session_sge,
                            .sge_count = 1,
                            .wr_len = sizeof(scp_transfer_end_t)};

  if (ctx->session_established) {
    scp_rdma_send_sync(ctx, &wr_end);
  }

  return 0;
}




