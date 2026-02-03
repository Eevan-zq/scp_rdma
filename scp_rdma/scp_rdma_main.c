/*
 * scp_rdma_main.c - SCP over RDMA 主程序入口
 */

#include "scp_rdma.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void scp_rdma_default_config(scp_rdma_config_t *cfg) {
  memset(cfg, 0, sizeof(scp_rdma_config_t));
  cfg->dev_name = SCP_DEFAULT_DEV_NAME;
  cfg->port = SCP_DEFAULT_PORT;
  cfg->ib_port = SCP_DEFAULT_IB_PORT;
  cfg->gid_idx = SCP_DEFAULT_GID_IDX;
  cfg->recursive = false;
  cfg->verbose = true;
  cfg->is_server = false;
  cfg->progress_cb = scp_default_progress;
}

void scp_default_progress(const char *filename, uint64_t transferred,
                          uint64_t total) {
  float percent = (total > 0) ? (float)transferred * 100 / total : 0;
  printf("\rProgress: [%-50s] %.1f%% (%lu/%lu bytes) %s",
         "==================================================" +
             (50 - (int)(percent / 2)),
         percent, transferred, total, filename);
  if (transferred >= total)
    printf("\n");
  fflush(stdout);
}

static void print_usage(const char *progname) {
  printf("Usage: %s [OPTIONS] SOURCE DEST\n", progname);
  printf("       %s -s [OPTIONS]\n\n", progname);
  printf("General Options:\n");
  printf("  -d <dev>    RDMA device name (e.g., mlx5_0)\n");
  printf("  -p <port>   TCP port for connection (default: %d)\n",
         SCP_DEFAULT_PORT);
  printf("  -i <port>   IB port (default: %d)\n", SCP_DEFAULT_IB_PORT);
  printf("  -g <idx>    GID index (default: %d)\n", SCP_DEFAULT_GID_IDX);
  printf("  -v          Verbose output\n");
  printf("  -h          Show this help\n\n");
  printf("Client Mode Options:\n");
  printf("  -r          Recursive directory transfer\n\n");
  printf("Server Mode Options:\n");
  printf("  -s          Run in server mode\n");
  printf("  -o <dir>    Output directory for received files\n\n");
}

int main(int argc, char *argv[]) {
  scp_rdma_config_t cfg;
  scp_rdma_default_config(&cfg);

  int opt;
  while ((opt = getopt(argc, argv, "d:p:i:g:vhsro:")) != -1) {
    switch (opt) {
    case 'd':
      cfg.dev_name = optarg;
      break;
    case 'p':
      cfg.port = atoi(optarg);
      break;
    case 'i':
      cfg.ib_port = atoi(optarg);
      break;
    case 'g':
      cfg.gid_idx = atoi(optarg);
      break;
    case 'v':
      cfg.verbose = true;
      break;
    case 's':
      cfg.is_server = true;
      break;
    case 'r':
      cfg.recursive = true;
      break;
    case 'o':
      cfg.output_dir = optarg;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  scp_rdma_context_t ctx;

  if (cfg.is_server) {
    if (scp_rdma_init(&ctx, &cfg))
      return 1;
    scp_receiver_run(&ctx, NULL);
    scp_rdma_destroy(&ctx);
  } else {
    if (optind + 1 >= argc) {
      fprintf(stderr,
              "Error: SOURCE and DEST must be specified in client mode\n");
      print_usage(argv[0]);
      return 1;
    }

    char *local_path = argv[optind];
    char *dest_str = argv[optind + 1];

    /* 解析 DEST 格式: [ip]:[remote_path] */
    char *colon = strchr(dest_str, ':');
    if (!colon) {
      fprintf(stderr, "Error: DEST should be in format ip:path\n");
      return 1;
    }
    *colon = '\0';
    cfg.server_addr = dest_str;
    char *remote_path = colon + 1;

    /* 如果 remote_path 是目录或以 / 结尾，追加本地文件名 */
    char final_remote_path[SCP_MAX_PATH_LEN];
    size_t rlen = strlen(remote_path);
    if (rlen == 0 || remote_path[rlen - 1] == '/' ||
        strcmp(remote_path, ".") == 0) {
      const char *base_name = strrchr(local_path, '/');
      if (base_name)
        base_name++;
      else
        base_name = local_path;

      while (rlen > 0 && remote_path[rlen - 1] == '/')
        rlen--;
      snprintf(final_remote_path, sizeof(final_remote_path), "%.*s/%s",
               (int)rlen, remote_path, base_name);
    } else {
      strncpy(final_remote_path, remote_path, sizeof(final_remote_path) - 1);
    }

    if (scp_rdma_init(&ctx, &cfg))
      return 1;

    printf("Starting transfer from %s to %s:%s\n", local_path, cfg.server_addr,
           final_remote_path);
    clock_t start = clock();

    if (scp_sender_run(&ctx, local_path, final_remote_path, NULL) == 0) {
      clock_t end = clock();
      double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
      printf("\nTransmission finished in %.2f seconds.\n", elapsed);
      printf("Total Files: %u, Total Dirs: %u, Total Data: %lu bytes\n",
             ctx.total_files, ctx.total_dirs, ctx.total_bytes);
    } else {
      fprintf(stderr, "\nTransmission failed!\n");
    }

    scp_rdma_destroy(&ctx);
  }

  return 0;
}

