/*
 * scp_rdma_protocol.h - SCP over RDMA 传输协议定义
 *
 * 定义了scp_rdma工具在文件传输过程中使用的各种协议结构体和常量
 */

#ifndef SCP_RDMA_PROTOCOL_H
#define SCP_RDMA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* 最大文件名长度 */
#define SCP_MAX_FILENAME_LEN 256

/* 最大路径长度 */
#define SCP_MAX_PATH_LEN 4096

/* 单次传输的最大数据块大小 (256MB) - 增大以提升大文件传输性能 */
#define SCP_CHUNK_SIZE (256 * 1024 * 1024)
/* 合并缓冲区大小：头信息 + 数据块 */
#define SCP_COMBINED_CHUNK_SIZE (SCP_CHUNK_SIZE + 1024)

/* 协议魔数，用于验证数据包 */
#define SCP_PROTOCOL_MAGIC 0x53435052 /* "SCPR" in ASCII */

/* 协议版本 */
#define SCP_PROTOCOL_VERSION 1

/* 命令类型枚举 */
typedef enum {
  SCP_CMD_NONE = 0,         /* 无效命令 */
  SCP_CMD_FILE_META = 1,    /* 文件元数据 - 包含文件名、大小等信息 */
  SCP_CMD_FILE_DATA = 2,    /* 文件数据块 */
  SCP_CMD_DIR_ENTER = 3,    /* 进入目录 - 开始传输新目录 */
  SCP_CMD_DIR_LEAVE = 4,    /* 离开目录 - 当前目录传输完成 */
  SCP_CMD_TRANSFER_END = 5, /* 整个传输结束 */
  SCP_CMD_ERROR = 6,        /* 错误消息 */
  SCP_CMD_ACK = 7           /* 确认消息 */
} scp_cmd_type_t;

/* 文件类型 */
typedef enum {
  SCP_FILE_REGULAR = 0,   /* 普通文件 */
  SCP_FILE_DIRECTORY = 1, /* 目录 */
  SCP_FILE_SYMLINK = 2    /* 符号链接 */
} scp_file_type_t;

/* 传输状态 */
typedef enum {
  SCP_STATE_IDLE = 0,       /* 空闲 */
  SCP_STATE_CONNECTING = 1, /* 正在连接 */
  SCP_STATE_READY = 2,      /* 已就绪 */
  SCP_STATE_SENDING = 3,    /* 正在发送 */
  SCP_STATE_RECEIVING = 4,  /* 正在接收 */
  SCP_STATE_COMPLETED = 5,  /* 已完成 */
  SCP_STATE_ERROR = 6       /* 出错 */
} scp_state_t;

/*
 * 协议头 - 每个传输包的开头
 * 用于标识数据包类型和基本信息
 */
typedef struct {
  uint32_t magic;       /* 魔数，用于验证 */
  uint8_t version;      /* 协议版本 */
  uint8_t cmd_type;     /* 命令类型 (scp_cmd_type_t) */
  uint16_t flags;       /* 标志位 */
  uint32_t payload_len; /* 负载长度 */
  uint32_t seq_num;     /* 序列号 */
} __attribute__((packed)) scp_packet_header_t;

/*
 * 文件元数据 - 描述一个文件的属性
 */
typedef struct {
  scp_packet_header_t header;           /* 协议头 */
  char file_name[SCP_MAX_FILENAME_LEN]; /* 文件名 */
  uint64_t file_size;                   /* 文件大小（字节） */
  uint32_t file_mode;                   /* 文件权限 (如 0644) */
  uint8_t file_type;                    /* 文件类型 (scp_file_type_t) */
  uint8_t reserved[3];                  /* 保留字节，用于对齐 */
  uint32_t total_chunks;                /* 总数据块数 */
  uint64_t mtime;                       /* 修改时间戳 */
} __attribute__((packed)) scp_file_meta_t;

/*
 * 数据块头 - 描述一个数据块
 */
typedef struct {
  scp_packet_header_t header; /* 协议头 */
  uint32_t chunk_index;       /* 当前块索引（从0开始）*/
  uint32_t chunk_size;        /* 当前块实际大小 */
  uint64_t offset;            /* 在文件中的偏移量 */
  uint32_t checksum;          /* 数据校验和 (CRC32) */
} __attribute__((packed)) scp_data_chunk_t;

/*
 * 目录信息 - 描述一个目录
 */
typedef struct {
  scp_packet_header_t header;          /* 协议头 */
  char dir_name[SCP_MAX_FILENAME_LEN]; /* 目录名 */
  uint32_t dir_mode;                   /* 目录权限 */
  uint32_t num_entries;                /* 目录下的条目数量 */
} __attribute__((packed)) scp_dir_info_t;

/*
 * 错误消息
 */
typedef struct {
  scp_packet_header_t header; /* 协议头 */
  int32_t error_code;         /* 错误码 */
  char error_msg[256];        /* 错误描述 */
} __attribute__((packed)) scp_error_msg_t;

/*
 * 传输结束确认
 */
typedef struct {
  scp_packet_header_t header; /* 协议头 */
  uint64_t total_bytes;       /* 总传输字节数 */
  uint32_t total_files;       /* 总文件数 */
  uint32_t total_dirs;        /* 总目录数 */
  uint32_t status;            /* 传输状态 0=成功 */
} __attribute__((packed)) scp_transfer_end_t;

/* 辅助函数声明 */

/**
 * 初始化协议头
 */
static inline void scp_init_packet_header(scp_packet_header_t *header,
                                          scp_cmd_type_t cmd_type,
                                          uint32_t payload_len) {
  header->magic = SCP_PROTOCOL_MAGIC;
  header->version = SCP_PROTOCOL_VERSION;
  header->cmd_type = (uint8_t)cmd_type;
  header->flags = 0;
  header->payload_len = payload_len;
  header->seq_num = 0;
}

/**
 * 验证协议头是否有效
 */
static inline int scp_validate_header(const scp_packet_header_t *header) {
  if (header->magic != SCP_PROTOCOL_MAGIC) {
    return -1; /* 魔数不匹配 */
  }
  if (header->version != SCP_PROTOCOL_VERSION) {
    return -2; /* 版本不匹配 */
  }
  return 0; /* 有效 */
}

/**
 * 计算CRC32校验和（简化版本）
 */
static inline uint32_t scp_calc_checksum(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFF;
  size_t i;

  for (i = 0; i < len; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}

#endif /* SCP_RDMA_PROTOCOL_H */
