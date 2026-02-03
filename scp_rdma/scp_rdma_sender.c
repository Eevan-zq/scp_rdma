/*
 * scp_rdma_sender.c - SCP over RDMA 发送端实现
 */

#include "scp_rdma.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 内部辅助函数：发送一个RDMA请求并等待同步 (Prepare ACK -> Write Data -> Signal
 * -> Wait ACK) */
static int scp_rdma_send_sync(scp_rdma_context_t *ctx, rdma_trans_wr_t *wr) {
  /* 1. 先准备好接收 ACK 的“口袋” */
  /* 这消除了 Race Condition：确保我们在触发对方之前，就已经在等待它的回复了 */
  if (post_receive_cdc(ctx->res.cdc_mr->addr, ctx->res.qp, ctx->res.cdc_mr)) {
    fprintf(stderr, "Failed to post ACK receive\n");
    return -1;
  }

  /* 2. RDMA Write (数据/元数据) */
  if (rdma_trans_post(&ctx->res, wr, &ctx->rdma_config)) {
    fprintf(stderr, "Failed to post RDMA WR\n");
    return -1;
  }

  if (rdma_trans_completion(&ctx->res)) {
    fprintf(stderr, "Failed to get RDMA completion\n");
    return -1;
  }

  /* 3. 发送 Data Ready 信号 */
  /* 注意：由于 post_send_cdc 现在使用 unsignaled send，我们不再需要等待它的
   * Send completion， 这避免了在共享 CQ 中多个完成事件交织导致的同步死锁。 */
  if (post_send_cdc(ctx->res.cdc_mr->addr, ctx->res.qp, ctx->res.cdc_mr)) {
    fprintf(stderr, "Failed to post CDC sync\n");
    return -1;
  }

  /* 4. 等待接收端的 ACK 信号 (通过第1步准备好的口袋接收) */
  if (rdma_cdc_completion(ctx->res.cq)) {
    fprintf(stderr, "Failed to receive ACK signal\n");
    return -1;
  }

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

  /* 初始化基础 RDMA 上下文 */
  if (rdma_context_init(&ctx->res, &ctx->rdma_config))
    return -1;
  if (pd_create(&ctx->res))
    return -1;
  if (cq_create(&ctx->res, CQ_MAX_SIZE))
    return -1;
  if (qp_create(&ctx->res, QP_MAX_SEND_WR, QP_MAX_RECV_WR, QP_MAX_SEND_SGE,
                QP_MAX_RECV_SGE))
    return -1;

  /* 预分配持久会话缓冲区 (但不立即注册 MR，由第一次传输流程触发) */
  ctx->session_buf = malloc(SCP_COMBINED_CHUNK_SIZE);
  if (!ctx->session_buf)
    return -1;
  memset(ctx->session_buf, 0, SCP_COMBINED_CHUNK_SIZE);

  ctx->session_sge = (rdma_trans_sge_t *)malloc(sizeof(rdma_trans_sge_t));
  memset(ctx->session_sge, 0, sizeof(rdma_trans_sge_t));
  ctx->session_sge->buf = ctx->session_buf;
  ctx->session_sge->buf_len =
      SCP_COMBINED_CHUNK_SIZE; /* 必须设置，供 mr_create 使用 */

  ctx->state = SCP_STATE_READY;
  ctx->session_established = false;
  return 0;
}

static int scp_prepare_transfer_resources(scp_rdma_context_t *ctx,
                                          rdma_trans_wr_t *wr, size_t len) {
  /* 关键修正：对于持久化会话，必须以最大可能的大小（SCP_COMBINED_CHUNK_SIZE）进行注册和握手
   */
  /* 即使当前 meta 包很小，MR 也必须覆盖后续 1MB 的数据块 */
  wr->wr_len = SCP_COMBINED_CHUNK_SIZE;

  /* sock_established 会交换长度并触发对端分配内存 */
  if (sock_established(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  /* rdma_trans_init 负责注册 MR 和连接 QP */
  if (rdma_trans_init(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  return 0;
}

/* 内部辅助：清理传输资源 - 已废弃，持久模式下统一销毁 */

int scp_send_file(scp_rdma_context_t *ctx, const char *local_path,
                  const char *remote_path) {
  struct stat st;
  if (stat(local_path, &st)) {
    perror("stat");
    return -1;
  }

  int fd = open(local_path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return -1;
  }

  /* 1. 发送文件元数据 */
  scp_file_meta_t *meta = (scp_file_meta_t *)ctx->session_buf;
  memset(meta, 0, sizeof(scp_file_meta_t));
  scp_init_packet_header(&meta->header, SCP_CMD_FILE_META,
                         sizeof(scp_file_meta_t) - sizeof(scp_packet_header_t));

  strncpy(meta->file_name, remote_path, SCP_MAX_FILENAME_LEN - 1);
  meta->file_size = st.st_size;
  meta->file_mode = st.st_mode;
  meta->file_type = SCP_FILE_REGULAR;
  meta->total_chunks = (st.st_size + SCP_CHUNK_SIZE - 1) / SCP_CHUNK_SIZE;

  /* 注意：在持久模式下，session_sge->buf_len 在 scp_rdma_init 中已设为 1MB */
  /* 我们通过 wr_meta.wr_len 控制初次握手的交换长度 */
  rdma_trans_wr_t wr_meta = {.op_type = RDMA_OP_WRITE,
                             .sges = ctx->session_sge,
                             .sge_count = 1,
                             .wr_len = sizeof(scp_file_meta_t)};

  /* 建立会话或复用连接 */
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

    /* 注意：以前这里需要手动 post_receive_cdc，
     * 现在已移入 scp_rdma_send_sync 内部自动处理，更加鲁棒 */
  }

  /* 在实际发送前，设置本次传输的具体长度。
   * 虽然 MR 是 1MB 的，但我们只需要发送 sizeof(scp_file_meta_t) */
  ctx->session_sge->buf_len = sizeof(scp_file_meta_t);
  if (scp_rdma_send_sync(ctx, &wr_meta)) {
    close(fd);
    return -1;
  }

  /* 2. 分块发送文件数据 */
  /* 关键修复：将 total_chunks 缓存到本地变量，因为 session_buf
   * 在发送数据块时会被覆盖 */
  uint32_t total_chunks = meta->total_chunks;
  uint64_t file_size = meta->file_size;
  uint64_t offset = 0;
  for (uint32_t i = 0; i < total_chunks; i++) {
    char *data_ptr = (char *)ctx->session_buf + sizeof(scp_data_chunk_t);
    ssize_t n = read(fd, data_ptr, SCP_CHUNK_SIZE);
    if (n <= 0)
      break;

    scp_data_chunk_t *data_header = (scp_data_chunk_t *)ctx->session_buf;
    memset(data_header, 0, sizeof(scp_data_chunk_t));
    scp_init_packet_header(&data_header->header, SCP_CMD_FILE_DATA,
                           n + sizeof(scp_data_chunk_t) -
                               sizeof(scp_packet_header_t));
    data_header->chunk_index = i;
    data_header->chunk_size = (uint32_t)n;
    data_header->offset = offset;

    ctx->session_sge->buf_len = sizeof(scp_data_chunk_t) + n;
    rdma_trans_wr_t wr_data = {.op_type = RDMA_OP_WRITE,
                               .sges = ctx->session_sge,
                               .sge_count = 1,
                               .wr_len = ctx->session_sge->buf_len};

    if (scp_rdma_send_sync(ctx, &wr_data)) {
      fprintf(stderr, "Failed to send chunk %u\n", i);
      close(fd);
      return -1;
    }

    offset += n;
    ctx->total_bytes += n;
    if (ctx->config.progress_cb) {
      ctx->config.progress_cb(local_path, offset, st.st_size);
    }
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

  /* 1. 发送进入目录指令 */
  scp_dir_info_t *heap_dir = (scp_dir_info_t *)ctx->session_buf;
  memset(heap_dir, 0, sizeof(scp_dir_info_t));
  scp_init_packet_header(&heap_dir->header, SCP_CMD_DIR_ENTER,
                         sizeof(scp_dir_info_t) - sizeof(scp_packet_header_t));
  strncpy(heap_dir->dir_name, remote_path, SCP_MAX_FILENAME_LEN - 1);

  /* 注意：持久模式下 session_sge->buf_len 在 init 中已设为 1MB */
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

  /* 实际发送前设置长度 */
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
    if (stat(sub_local, &st))
      continue;

    if (S_ISDIR(st.st_mode)) {
      if (ctx->config.recursive) {
        if (scp_send_directory(ctx, sub_local, sub_remote)) {
          closedir(dir);
          return -1;
        }
      }
    } else {
      if (scp_send_file(ctx, sub_local, sub_remote)) {
        closedir(dir);
        return -1;
      }
    }
  }

  /* 3. 发送离开目录指令 */
  scp_dir_info_t *heap_leave = (scp_dir_info_t *)ctx->session_buf;
  memset(heap_leave, 0, sizeof(scp_dir_info_t));
  scp_init_packet_header(&heap_leave->header, SCP_CMD_DIR_LEAVE, 0);

  ctx->session_sge->buf_len = sizeof(scp_packet_header_t);
  rdma_trans_wr_t wr_leave = {.op_type = RDMA_OP_WRITE,
                              .sges = ctx->session_sge,
                              .sge_count = 1,
                              .wr_len = sizeof(scp_packet_header_t)};

  if (scp_rdma_send_sync(ctx, &wr_leave)) {
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
  if (S_ISDIR(st.st_mode)) {
    ret = scp_send_directory(ctx, local_path, remote_path);
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
