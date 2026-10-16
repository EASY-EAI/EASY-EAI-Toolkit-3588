/**
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * 日志管理模块
 *
 * 使用方式：
 *   1. log_mgr_t mgr = log_manager_init("/userdata/logs/app_cfg.ini");  // 初始化（必须指定配置文件）
 *   2. log_handle_t h = log_register(mgr, "gst");       // 注册模块
 *   3. PRINT_INFO(h, "hello\n");                         // 打印日志
 *
 * 多管理器（各自独立配置，互不干扰）：
 *   log_mgr_t mgr_a = log_manager_init("/userdata/logs/a_cfg.ini");
 *   log_handle_t h1 = log_register(mgr_a, "someip");
 *   log_mgr_t mgr_b = log_manager_init("/userdata/logs/b_cfg.ini");
 *   log_handle_t h2 = log_register(mgr_b, "server");
 *
 * 运行时控制（CLI）：
 *   log                       列出所有运行中的管理器
 *   log <mgr>                列出指定管理器的所有模块
 *   log <mgr> gst=debug      设置指定管理器的 gst 为 debug
 *   log gst=debug            单管理器时直接设置
 *
 * 级别：none | error | warn | info | debug
 */

#ifndef LOG_MANAGER_PRO_H
#define LOG_MANAGER_PRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 日志级别 */
#define LOG_LEVEL_NONE    0
#define LOG_LEVEL_ERROR   1
#define LOG_LEVEL_WARN    2
#define LOG_LEVEL_INFO    3
#define LOG_LEVEL_DEBUG   4

/* 容量限制（编译前可按需修改） */
#define MAX_MGRS        4       /* 最大管理器实例数，每次 log_manager_init 占用一个 */
#define MAX_MODULES     32      /* 最大模块注册数，每次 log_register 占用一个 */

/* 句柄类型 */
typedef int32_t log_mgr_t;
#define LOG_MGR_INVALID    (-1)

typedef int32_t log_handle_t;
#define LOG_HANDLE_INVALID (-1)

/* 初始化日志管理器，返回管理器句柄
 * configFile 必须为非 NULL 的有效路径（如 "/userdata/logs/app_cfg.ini"）
 * 传 NULL 或空字符串将返回 LOG_MGR_INVALID
 * 同一进程可多次调用，创建多个独立管理器实例
 * 若 configFile 与已注册的管理器相同，则返回已有管理器句柄（避免 socket 冲突）
 * 配置文件名即管理器名称，CLI 通过 log <mgr> 指定控制目标 */
log_mgr_t log_manager_init(const char *configFile);

/* 在指定管理器下注册模块，返回模块句柄（>=0 成功，<0 失败）
 * 同一管理器下同名模块重复注册返回已有句柄 */
log_handle_t log_register(log_mgr_t mgr, const char *moduleName);

/*
 * 打印函数，一般通过 PRINT_* 宏调用。
 *
 * 注意：格式化后的消息长度上限为 511 字节，超出部分会被静默截断。
 * 如需输出超长内容，请在调用方自行分段。
 */
void log_print(int level, log_handle_t handle, const char *fmt, ...);

#define PRINT_ERROR(handle, fmt, args...) log_print(LOG_LEVEL_ERROR, handle, fmt, ##args)
#define PRINT_WARN(handle, fmt, args...)  log_print(LOG_LEVEL_WARN,  handle, fmt, ##args)
#define PRINT_INFO(handle, fmt, args...)  log_print(LOG_LEVEL_INFO,  handle, fmt, ##args)
#define PRINT_DEBUG(handle, fmt, args...) log_print(LOG_LEVEL_DEBUG, handle, fmt, ##args)

#ifdef __cplusplus
}
#endif

#endif /* LOG_MANAGER_PRO_H */
