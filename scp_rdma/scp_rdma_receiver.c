/*
 * scp_rdma_receiver.c - SCP over RDMA 接收端实现
 */

#include "scp_rdma.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 内部辅助：递归创建目录 */
static int scp_mkdir_recursive(const char *path) {
  char tmp[SCP_MAX_PATH_LEN];
  char *p = NULL;
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) && errno != EEXIST) {
    return -1;
  }
  return 0;
}

/* 内部辅助：接收同步信号 - 已废弃，功能合并入主循环 */

static int scp_prepare_receive_resources(scp_rdma_context_t *ctx,
                                         rdma_trans_wr_t *wr) {
  /* Server 端需要循环处理请求 */
  if (!ctx->res.is_alive) {
    if (qp_create(&ctx->res, QP_MAX_SEND_WR, QP_MAX_RECV_WR, QP_MAX_SEND_SGE,
                  QP_MAX_RECV_SGE)) {
      return -1;
    }
  }

  /* 关键修正：在调用 sock_established 之前，必须确保 wr_len 为 0 */
  /* 这样 server 端才会真正从 TCP 接收 client 发送的握手长度 (1MB) */
  wr->wr_len = 0;

  /* sock_established 会等待 client 发送长度并准备本地缓存 */
  if (sock_established(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  /* rdma_trans_init 负责注册 MR 和连接 QP */
  if (rdma_trans_init(&ctx->res, wr, &ctx->rdma_config)) {
    return -1;
  }

  return 0;
}

/* 内部辅助：清理接收资源 - 已废弃，持久模式下统一销毁 */

int scp_receiver_run(scp_rdma_context_t *ctx, scp_result_t *result) {
  (void)result; /* 标记为未使用 */
  int current_fd = -1;
  char current_path[SCP_MAX_PATH_LEN] = {0};
  char base_dir[SCP_MAX_PATH_LEN] = {0};

  if (ctx->config.output_dir) {
    strncpy(base_dir, ctx->config.output_dir, sizeof(base_dir) - 1);
    scp_mkdir_recursive(base_dir);
  } else {
    if (!getcwd(base_dir, sizeof(base_dir))) {
      perror("getcwd");
      return -1;
    }
  }

  printf("Waiting for RDMA connection on port %u...\n", ctx->config.port);

  while (1) {
    rdma_trans_wr_t msg = {0};

    /* 1. 准备接收资源 */
    if (!ctx->session_established) {
      /* 重要：在第一次握手前指定会话缓冲区，诱导 mr_create 使用它而不是 malloc
       */
      msg.sge_count = 1;
      msg.sges = ctx->session_sge;

      if (scp_prepare_receive_resources(ctx, &msg)) {
        fprintf(stderr, "Failed to prepare receive resources\n");
        break;
      }
      /* 捕获注册后的 MR 以便后续复用 */
      ctx->session_mr = ctx->session_sge->mr;
      ctx->session_established = true;

      /* 初始化：第一次进入循环前，先发布接收 Data Ready 的请求 */
      /* 这样就不用 scp_rdma_wait_sync 内部去 post 了，避免了循环中的时序问题 */
      if (post_receive_cdc(ctx->res.cdc_mr->addr, ctx->res.qp,
                           ctx->res.cdc_mr)) {
        fprintf(stderr, "Failed to post initial Data Ready receive\n");
        break;
      }
    }

    /* 2. 等待同步信号，表示数据已写入完成 (Data Ready) */
    /* 注意：我们在上一轮循环末尾（或初始化时）已经调用了 post_receive_cdc */
    if (rdma_cdc_completion(ctx->res.cq)) {
      fprintf(stderr, "CDC sync failed\n");
      break;
    }

    /* 3. 解析协议头 (数据已经在 ctx->session_buf 中) */
    scp_packet_header_t *header = (scp_packet_header_t *)ctx->session_buf;
    if (scp_validate_header(header) != 0) {
      fprintf(stderr, "Header validation failed\n");
      /* 即使头部不对，也要尝试发送 ACK 解锁发送端，否则发送端会死锁，
       * 但这里为了简化错误处理，我们选择断开连接 */
      break;
    }

    /* 4. 根据命令处理 (全部从 ctx->session_buf 读取) */
    bool is_end_packet = (header->cmd_type == SCP_CMD_TRANSFER_END);
    if (header->cmd_type == SCP_CMD_FILE_META) {
      scp_file_meta_t *meta = (scp_file_meta_t *)ctx->session_buf;

      /* 关键修正：处理文件名开头的斜杠，防止生成的路径出现 // */
      const char *fname = meta->file_name;
      while (*fname == '/')
        fname++;

      size_t blen = strlen(base_dir);
      while (blen > 0 && base_dir[blen - 1] == '/')
        blen--;

      snprintf(current_path, sizeof(current_path), "%.*s/%s", (int)blen,
               base_dir, fname);

      struct stat target_st;
      if (stat(current_path, &target_st) == 0 && S_ISDIR(target_st.st_mode)) {
        fprintf(stderr, "Error: Target %s is a directory, cannot overwrite.\n",
                current_path);
        if (current_fd >= 0)
          close(current_fd);
        current_fd = -1;
      } else {
        char *last_slash = strrchr(current_path, '/');
        if (last_slash) {
          *last_slash = '\0';
          scp_mkdir_recursive(current_path);
          *last_slash = '/';
        }

        if (current_fd >= 0)
          close(current_fd);
        current_fd =
            open(current_path, O_WRONLY | O_CREAT | O_TRUNC, meta->file_mode);
        if (current_fd < 0) {
          perror("open failed");
        } else if (ctx->config.verbose) {
          printf("Receiving file: %s -> %s (%lu bytes)\n", fname, current_path,
                 meta->file_size);
        }
        ctx->total_files++;
      }
    } else if (header->cmd_type == SCP_CMD_FILE_DATA) {
      scp_data_chunk_t *chunk_info = (scp_data_chunk_t *)ctx->session_buf;
      void *data_ptr = (char *)ctx->session_buf + sizeof(scp_data_chunk_t);

      if (current_fd >= 0) {
        lseek(current_fd, chunk_info->offset, SEEK_SET);
        if (write(current_fd, data_ptr, chunk_info->chunk_size) !=
            (ssize_t)chunk_info->chunk_size) {
          perror("write");
        }
      }
      ctx->total_bytes += chunk_info->chunk_size;
    } else if (header->cmd_type == SCP_CMD_DIR_ENTER) {
      scp_dir_info_t *dir_info = (scp_dir_info_t *)ctx->session_buf;

      /* 统一路径处理：防止出现 // */
      const char *dname = dir_info->dir_name;
      while (*dname == '/')
        dname++;

      size_t blen = strlen(base_dir);
      while (blen > 0 && base_dir[blen - 1] == '/')
        blen--;

      snprintf(current_path, sizeof(current_path), "%.*s/%s", (int)blen,
               base_dir, dname);

      scp_mkdir_recursive(current_path);
      ctx->total_dirs++;
      if (ctx->config.verbose)
        printf("Created directory: %s\n", current_path);
    } else if (header->cmd_type == SCP_CMD_TRANSFER_END) {
      printf("\nTransfer completed successfully.\n");
      if (current_fd >= 0)
        close(current_fd);
      /* 结束包依然需要 ACK，否则发送端会超时 */
      // break;
    }

    /* 5. 关键修正：在发送 ACK 之前，先为下一轮 Data Ready 做好接收准备 */
    /* 这消除了 Race Condition：如果先发 ACK，发送端极速发送下一个 Data Ready
     * 时，接收端可能还没来得及 Post Recv */
    if (!is_end_packet) {
      if (post_receive_cdc(ctx->res.cdc_mr->addr, ctx->res.qp,
                           ctx->res.cdc_mr)) {
        fprintf(stderr, "Failed to post next Data Ready receive\n");
        break;
      }
    }

    /* 6. 发送 ACK 信号告诉发送端: "我处理完了，缓冲区安全了" */
    /* 注意：由于 post_send_cdc 现在使用 unsignaled send，我们不再需要等待它的
     * Send completion， 这彻底解决了在高并发/小文件传输下的同步死锁问题。 */
    if (post_send_cdc(ctx->res.cdc_mr->addr, ctx->res.qp, ctx->res.cdc_mr)) {
      fprintf(stderr, "Failed to send ACK\n");
      break;
    }

    /* 如果是结束包，发完 ACK 后再退出循环 */
    if (is_end_packet) {
      break;
    }
  }

  /* 5. 持久模式下不在此处清理资源，由 final destroy 处理 */
  // scp_cleanup_receive_resources(ctx, &msg);

  return 0;
}

void scp_rdma_destroy(scp_rdma_context_t *ctx) {
  if (ctx->session_mr) {
    ibv_dereg_mr(ctx->session_mr);
    ctx->session_mr = NULL;
  }
  if (ctx->session_buf) {
    free(ctx->session_buf);
    ctx->session_buf = NULL;
  }
  if (ctx->session_sge) {
    free(ctx->session_sge);
    ctx->session_sge = NULL;
  }

  if (ctx->res.cdc_mr) {
    ibv_dereg_mr(ctx->res.cdc_mr);
    ctx->res.cdc_mr = NULL;
  }
  qp_destroy(&ctx->res);
  cq_destroy(&ctx->res);
  pd_destroy(&ctx->res);
  if (ctx->res.ib_ctx) {
    ibv_close_device(ctx->res.ib_ctx);
    ctx->res.ib_ctx = NULL;
  }
  if (ctx->res.sock >= 0) {
    close(ctx->res.sock);
    ctx->res.sock = -1;
  }
}
