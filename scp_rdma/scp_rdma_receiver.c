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
/* 修复：移除末尾斜杠处理，防止死循环 */
static int scp_mkdir_recursive(const char *path) {
  char tmp[SCP_MAX_PATH_LEN];
  char *p = NULL;
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;

  /* 优化：先尝试直接创建，成功则无需遍历 */
  if (mkdir(tmp, 0755) == 0 || errno == EEXIST) {
    return 0;
  }

  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      /* 只有不存在时才根据 errno 判断是否真的失败 */
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
      int extra = ctx->res.cdc_recv_slots > 0 ? ctx->res.cdc_recv_slots - 1 : 0;
      for (int i = 0; i < extra; ++i) {
        rdma_cdc_t *buf = rdma_cdc_acquire_post_buf(&ctx->res);
        if (!buf ||
            post_receive_cdc(buf, ctx->res.qp, ctx->res.cdc_mr)) {
          fprintf(stderr, "Failed to extend CDC receive window\n");
          extra = -1;
          break;
        }
      }
      if (extra == -1) {
        break;
      }
      ctx->local_credits = ctx->res.cdc_recv_slots;
    }

    /* 2. 等待同步信号，表示数据已写入完成 (Data Ready) */
    /* 注意：我们在上一轮循环末尾（或初始化时）已经调用了 post_receive_cdc */
    if (rdma_cdc_completion(
            ctx->res.cq)) { /* - 这是while(1)的在ibv_poll_cq，控制命令都是同步的
                               - */
      fprintf(stderr, "CDC sync failed\n");
      break;
    }
    if (ctx->local_credits > 0) {
      ctx->local_credits--;
    }

    /* 3. 解析 slot_id 以确定数据位置（双缓冲支持） */
    rdma_cdc_t *cdc_recv = rdma_cdc_consume_buf(&ctx->res);
    if (!cdc_recv) {
      fprintf(stderr, "CDC consume failed\n");
      break;
    }
    int slot_id = cdc_recv->slot_id;
    fprintf(stderr, "[RECEIVER DEBUG] CDC signal received slot=%d\n", slot_id);
    /* 根据 slot_id 计算数据缓冲区基地址 */
    char *slot_base =
        (char *)ctx->session_buf + (slot_id * SCP_COMBINED_CHUNK_SIZE);

    /* DEBUG: 打印接收到的信息 */
    fprintf(stderr,
            "[RECEIVER DEBUG] slot_id=%d, slot_base=%p, session_buf=%p, "
            "offset=%llu\n",
            slot_id, (void *)slot_base, (void *)ctx->session_buf,
            (unsigned long long)(slot_id * SCP_COMBINED_CHUNK_SIZE));

    /* 4. 解析协议头 (数据在对应 slot 的缓冲区中) */
    scp_packet_header_t *header = (scp_packet_header_t *)slot_base;

    /* DEBUG: 打印 header 内容 */
    fprintf(stderr, "[RECEIVER DEBUG] header: magic=0x%x, cmd=%d, len=%u\n",
            header->magic, header->cmd_type, header->payload_len);

    if (scp_validate_header(header) != 0) {
      fprintf(stderr, "Header validation failed (slot %d)\n", slot_id);
      /* 即使头部不对，也要尝试发送 ACK 解锁发送端，否则发送端会死锁，
       * 但这里为了简化错误处理，我们选择断开连接 */
      break;
    }

    /* 5. 根据命令处理 (全部从 slot_base 读取) - 处理业务逻辑 - */
    bool is_end_packet = (header->cmd_type == SCP_CMD_TRANSFER_END);
    static int meta_type = SCP_FILE_REGULAR; /* 记录当前文件类型 */

    if (header->cmd_type == SCP_CMD_FILE_META) {
      scp_file_meta_t *meta = (scp_file_meta_t *)slot_base;
      meta_type = meta->file_type;

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
        current_fd = -1;

        if (meta_type != SCP_FILE_SYMLINK) {
          current_fd =
              open(current_path, O_WRONLY | O_CREAT | O_TRUNC, meta->file_mode);
          if (current_fd < 0) {
            perror("open failed");
          }
        }

        if (ctx->config.verbose) {
          if (meta_type == SCP_FILE_SYMLINK) {
            printf("Receiving symlink: %s -> %s\n", fname, current_path);
          } else {
            printf("Receiving file: %s -> %s (%llu bytes)\n", fname,
                   current_path, (unsigned long long)meta->file_size);
          }
        }
        if (meta_type != SCP_FILE_SYMLINK) {
          ctx->total_files++;
        }
      }
    } else if (header->cmd_type == SCP_CMD_FILE_DATA) {
      scp_data_chunk_t *chunk_info = (scp_data_chunk_t *)slot_base;
      void *data_ptr = (char *)slot_base + sizeof(scp_data_chunk_t);

      if (meta_type == SCP_FILE_SYMLINK) {
        /* 处理符号链接数据 (即目标路径) */
        char link_target[SCP_MAX_PATH_LEN];
        uint32_t len = chunk_info->chunk_size;
        if (len >= sizeof(link_target))
          len = sizeof(link_target) - 1;

        memcpy(link_target, data_ptr, len);
        link_target[len] = '\0';

        /* 先删除可能存在的旧文件/链接 */
        unlink(current_path);

        if (symlink(link_target, current_path)) {
          perror("symlink failed");
        } else {
          if (ctx->config.verbose) {
            printf("Created symlink: %s -> %s\n", current_path, link_target);
          }
          ctx->total_symlinks++;
        }
        /* 软连接不需要 close fd，因为它没有打开 fd */
      } else if (current_fd >= 0) {
        /* 普通文件写入 */
        lseek(current_fd, chunk_info->offset, SEEK_SET);
        if (write(current_fd, data_ptr, chunk_info->chunk_size) !=
            (ssize_t)chunk_info->chunk_size) {
          perror("write");
        }
        ctx->total_bytes += chunk_info->chunk_size;
      }
    } else if (header->cmd_type == SCP_CMD_DIR_ENTER) {
      scp_dir_info_t *dir_info = (scp_dir_info_t *)slot_base;

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
    } else if (header->cmd_type == SCP_CMD_DIR_LEAVE) {
      if (ctx->config.verbose)
        printf("Leaving directory.\n");
      if (current_fd >= 0) {
        close(current_fd);
        current_fd = -1;
      }
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
      rdma_cdc_t *next_buf = rdma_cdc_acquire_post_buf(&ctx->res);
      if (!next_buf) {
        fprintf(stderr, "Failed to allocate CDC receive buffer\n");
        break;
      }
      if (post_receive_cdc(next_buf, ctx->res.qp, ctx->res.cdc_mr)) {
        fprintf(stderr, "Failed to post next Data Ready receive\n");
        break;
      }
      ctx->local_credits++;
    }

    /* 6. 发送 ACK 信号告诉发送端: "我处理完了，缓冲区安全了" */
    /* 双缓冲优化：将收到的 slot_id 回传给发送端，让发送端知道哪个 slot 安全了
     */
    rdma_cdc_t *cdc_signal = ctx->res.cdc_send_buf;
    memcpy(cdc_signal, cdc_recv, sizeof(*cdc_signal));
    cdc_signal->slot_id = (uint8_t)slot_id;
    cdc_signal->signal_type = 1; /* ACK */
    cdc_signal->credits = (uint16_t)ctx->local_credits;
    cdc_signal->server_is_confirmed = 1;
    fprintf(stderr, "[RECEIVER DEBUG] send ACK slot=%d\n", slot_id);

    /* 注意：post_send_cdc 使用 SIGNALED+INLINE 发送，
     * 但发送端的 harvest_completions 会正确区分 Send WC 和 Recv WC。 */
    if (post_send_cdc(ctx->res.cdc_send_buf, ctx->res.qp, ctx->res.cdc_mr)) {
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
  fprintf(stderr, "[DESTROY DEBUG] Starting cleanup...\n");

  /* 清理双缓冲资源 - 注意：slot_buf 可能是 session_buf 的偏移，不能单独 free
   */
  for (int i = 0; i < DUAL_BUFFER_SLOTS; i++) {
    /* 只有当 slot_mr 独立注册时才注销 */
    if (ctx->slot_mr[i] && ctx->slot_mr[i] != ctx->session_mr) {
      fprintf(stderr, "[DESTROY DEBUG] Deregistering slot_mr[%d]...\n", i);
      ibv_dereg_mr(ctx->slot_mr[i]);
    }
    ctx->slot_mr[i] = NULL;

    /* 检查 slot_buf 是否是 session_buf 的偏移 */
    if (ctx->slot_buf[i] && ctx->session_buf) {
      char *session_start = (char *)ctx->session_buf;
      char *session_end = session_start + SCP_DUAL_BUFFER_TOTAL_SIZE;
      char *slot_ptr = (char *)ctx->slot_buf[i];

      if (slot_ptr < session_start || slot_ptr >= session_end) {
        fprintf(stderr, "[DESTROY DEBUG] Freeing independent slot_buf[%d]...\n",
                i);
        free(ctx->slot_buf[i]);
      } else {
        fprintf(
            stderr,
            "[DESTROY DEBUG] slot_buf[%d] is session_buf offset, skip free\n",
            i);
      }
    }
    ctx->slot_buf[i] = NULL;
  }

  if (ctx->session_mr) {
    fprintf(stderr, "[DESTROY DEBUG] Deregistering session_mr...\n");
    ibv_dereg_mr(ctx->session_mr);
    ctx->session_mr = NULL;
  }
  if (ctx->session_buf) {
    fprintf(stderr, "[DESTROY DEBUG] Freeing session_buf...\n");
    free(ctx->session_buf);
    ctx->session_buf = NULL;
  }
  if (ctx->session_sge) {
    fprintf(stderr, "[DESTROY DEBUG] Freeing session_sge...\n");
    free(ctx->session_sge);
    ctx->session_sge = NULL;
  }

  if (ctx->res.cdc_mr) {
    fprintf(stderr, "[DESTROY DEBUG] Deregistering cdc_mr...\n");
    ibv_dereg_mr(ctx->res.cdc_mr);
    ctx->res.cdc_mr = NULL;
  }

  fprintf(stderr, "[DESTROY DEBUG] Destroying QP...\n");
  qp_destroy(&ctx->res);

  fprintf(stderr, "[DESTROY DEBUG] Destroying CQ...\n");
  cq_destroy(&ctx->res);

  fprintf(stderr, "[DESTROY DEBUG] Destroying PD...\n");
  pd_destroy(&ctx->res);

  if (ctx->res.ib_ctx) {
    fprintf(stderr, "[DESTROY DEBUG] Closing IB device...\n");
    ibv_close_device(ctx->res.ib_ctx);
    ctx->res.ib_ctx = NULL;
  }
  if (ctx->res.sock >= 0) {
    fprintf(stderr, "[DESTROY DEBUG] Closing socket...\n");
    close(ctx->res.sock);
    ctx->res.sock = -1;
  }
  fprintf(stderr, "[DESTROY DEBUG] Cleanup complete.\n");
}





