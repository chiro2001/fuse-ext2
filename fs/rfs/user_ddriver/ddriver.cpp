#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <linux/fs.h>
#include "ddriver_ctl.hpp"
#include <cstdio>
#include <cerrno>
#include <pwd.h>
#include <ctime>

extern int errno;

#define USER_INFO     "INFO: "
#define USER_ALERT    "WARNING: "

#define USER_PANIC    "PANIC: "
/******************************************************************************
* SECTION: Macro definitions
*******************************************************************************/
#define DEVICE_NAME   "ddriver"
#define DEVICE_LOG    "ddriver_log"

#define user_info(fmt, ...)\
  do {\
    printf(USER_INFO DEVICE_NAME " " fmt "\n", ##__VA_ARGS__);\
        fprintf(debugf, USER_PANIC  " " fmt "\n", ##__VA_ARGS__);\
  } while(0)\

#define user_alert(fmt, ...)\
  do {\
    printf(USER_ALERT DEVICE_NAME " " fmt "\n", ##__VA_ARGS__);\
        fprintf(debugf, USER_PANIC  " " fmt "\n", ##__VA_ARGS__);\
  } while(0)\

#define user_panic(fmt, ...)\
    do {\
        printf(USER_PANIC  " " fmt "\n", ##__VA_ARGS__);\
    } while (0)\

#define DRIVER_AUTHOR   "Deadpool <deadpoolmine@qq.com>"
#define DRIVER_DESC     "A Fake disk driver in user space"
#define DRIVER_VERSION  "0.1.0"

#define CONFIG_DISK_SZ  (4 * 1024 * 1024)
#define CONFIG_BLOCK_SZ (512)
/******************************************************************************
* SECTION: Macro Functions 
*******************************************************************************/
#define IGNORE_ARG(arg)         ((void)arg)
#define IS_ADDR_ALIGN(addr)     (addr % CONFIG_BLOCK_SZ == 0)
#define ADDR_ROUND_UP(addr)     ((addr / CONFIG_BLOCK_SZ) * CONFIG_BLOCK_SZ)

#define INC_READCNT(disk)       (disk.read_cnt++)
#define INC_WRITECNT(disk)      (disk.write_cnt++)
#define INC_SEEKCNT(disk)       (disk.seek_cnt++)

#define RW_DELAY(disk, rw_ops)  (usleep(disk.rw_ops##_lat * 1000))
/******************************************************************************
* SECTION: Type definitions
*******************************************************************************/
struct ddriver {
  int ddriver_fd;                                 /* Disk ddriver_fd */
  int read_cnt;
  int write_cnt;
  int seek_cnt;
  int read_lat;
  int write_lat;
  int seek_lat;
  int track_num;
  int major_num;
  int layout_size;
  int iounit_size;
};
/******************************************************************************
* SECTION: Global Variable
*******************************************************************************/
/* reference: https://en.wikipedia.org/wiki/Hard_disk_drive_performance_characteristics */
struct ddriver disk = {
        .read_cnt    = 0,
        .write_cnt   = 0,
        .seek_cnt    = 0,
        .read_lat    = 2,       /* 2ms */
        .write_lat   = 1,       /* 1ms */
        .seek_lat    = 4,       /* 4.17ms per 360 degree */
        .track_num   = 100,
        .major_num   = 0,
        .layout_size = CONFIG_DISK_SZ,
        .iounit_size = CONFIG_BLOCK_SZ
};

FILE *debugf = NULL;

/******************************************************************************
* SECTION: Helper Functions
*******************************************************************************/
int check_valid(size_t size) {
  if (size != CONFIG_BLOCK_SZ) {
    user_alert("io size %ld should align to %d", size, CONFIG_BLOCK_SZ);
    return -EIO;
  }
  return 0;
}

int emulate_rotate(int fd, off_t start, off_t end) {
  int bytes_per_track = disk.layout_size / disk.track_num;
  int lat_per_track = disk.seek_lat;
  int distance = abs(end - start) % bytes_per_track;

  if (distance == 0) {
    return 0;
  }

  usleep(distance * lat_per_track / bytes_per_track * 1000);
  return 0;
}
/******************************************************************************
* SECTION: Global Function Implementation
*******************************************************************************/
/**
 * @brief 打开驱动
 * 
 * @return int 文件描述符
 */
int ddriver_open(char *path) {
  int fd, ret = 0;
  char device_path[128] = {0};
  char log_path[128] = {0};

  sprintf(device_path, "%s/" DEVICE_NAME, getpwuid(getuid())->pw_dir);
  sprintf(log_path, "%s/" DEVICE_LOG, getpwuid(getuid())->pw_dir);

  if (strcmp(device_path, path) != 0) {
    user_panic("wrong path [%s], should be [%s]", path, device_path);
    return -1;
  }

  if (access(device_path, F_OK) == 0) {
    fd = open(device_path, O_RDWR);
  } else {
    fd = open(device_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  }
  if (fd < 0) {
    user_panic("can't open device: %d", fd);
    return fd;
  }
  ret = posix_fallocate(fd, 0, CONFIG_DISK_SZ);
  if (ret < 0) {
    user_panic("low space");
    return ret;
  }

  debugf = fopen(log_path, "w+");
  if (debugf == NULL) {
    user_panic("can't init log: %s", log_path);
    return -1;
  }

  return fd;
}

/**
 * @brief 关闭驱动
 * 
 * @param fd 
 * @return int 
 */
int ddriver_close(int fd) {
  return close(fd) && fclose(debugf);
}

/**
 * @brief 磁盘头SEEK
 * 
 * @param fd 
 * @param offset 
 * @param whence 
 * @return int 
 */
int ddriver_seek(int fd, off_t offset, int whence) {
  int ret = 0;
  int cur = 0;

  if (!IS_ADDR_ALIGN(offset)) {
    user_alert("offset %ld must be aligned to block size %d",
               offset, CONFIG_BLOCK_SZ);
    return -EINVAL;
  }

  INC_SEEKCNT(disk);
  cur = lseek(fd, 0, SEEK_CUR);
  ret = lseek(fd, offset, whence);
  if (ret < 0) {
    user_panic("seek error: %s", strerror(errno));
    return ret;
  }
  emulate_rotate(fd, cur, ret);
  return ret;
}

/**
 * @brief 磁盘写入，写入大小可通过IOCTL查询
 * 
 * @param fd 
 * @param buf 
 * @param size 
 * @return int 
 */
int ddriver_write(int fd, char *buf, size_t size) {
  int res = check_valid(size);
  if (res < 0)
    return res;

  RW_DELAY(disk, write);
  write(fd, buf, size);

  INC_WRITECNT(disk);
  return CONFIG_BLOCK_SZ;
}

/**
 * @brief 
 * 
 * @param fd 
 * @param buf 
 * @param size 
 * @return int 
 */
int ddriver_read(int fd, char *buf, size_t size) {
  int res = check_valid(size);
  if (res < 0)
    return res;

  RW_DELAY(disk, read);
  read(fd, buf, size);

  INC_READCNT(disk);
  return CONFIG_BLOCK_SZ;
}

/**
 * @brief 
 * 
 * @param fd 
 * @param cmd 
 * @param arg 
 * @return int 
 */
int ddriver_ioctl(int fd, unsigned long cmd, void *arg) {
  struct ddriver_state state{};
  char buf[4096] = {'\0'};
  switch (cmd) {
    case IOC_REQ_DEVICE_SIZE:                         /* Device Size */
      memcpy(arg, &disk.layout_size, sizeof(int));
      break;
    case IOC_REQ_DEVICE_STATE:                        /* Device State */
      state.read_cnt = disk.read_cnt;
      state.write_cnt = disk.write_cnt;
      state.seek_cnt = disk.seek_cnt;
      memcpy(arg, &state, sizeof(struct ddriver_state));
      break;
    case IOC_REQ_DEVICE_RESET:                        /* Reset Device */
      lseek(fd, 0, SEEK_SET);
      for (size_t i = 0; i < CONFIG_DISK_SZ; i += 4096) {
        write(fd, buf, 4096);
      }
      lseek(fd, 0, SEEK_SET);
      disk.read_cnt = 0;
      disk.write_cnt = 0;
      disk.seek_cnt = 0;
      break;
    case IOC_REQ_DEVICE_IO_SZ:
      memcpy(arg, &disk.iounit_size, sizeof(int));
      break;
    default:
      break;
  }
  return 0;
}