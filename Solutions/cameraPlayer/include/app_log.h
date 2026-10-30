#ifndef APP_LOG_H
#define APP_LOG_H

#include "log_manager_pro.h"

/*
 * 日志句柄分两层：
 *   - 应用层（main / video_stitch / display_watch）共用进程级 g_log，
 *     由入口在 log_manager_init 后 log_register 填充；
 *   - 模块层（gpu / rga / camera）各自持有句柄：模块在自己的 init 函数里
 *     用 app_log_register_module() 注册（句柄存模块内），不依赖 g_log。
 *
 * 模块源文件绑定自己的句柄（须在 include 本头文件前定义）：
 *   static log_handle_t s_log = LOG_HANDLE_INVALID;
 *   #define APP_LOG_HANDLE s_log
 *   #include "app_log.h"
 *
 * 句柄为 LOG_HANDLE_INVALID 时 LOG_* 自动降为空操作。
 */

#ifndef APP_LOG_HANDLE
extern log_handle_t g_log;
#define APP_LOG_HANDLE g_log
#endif

#define LOG_E(fmt, args...) do { if (APP_LOG_HANDLE != LOG_HANDLE_INVALID) PRINT_ERROR(APP_LOG_HANDLE, fmt, ##args); } while (0)
#define LOG_W(fmt, args...) do { if (APP_LOG_HANDLE != LOG_HANDLE_INVALID) PRINT_WARN(APP_LOG_HANDLE, fmt, ##args); } while (0)
#define LOG_I(fmt, args...) do { if (APP_LOG_HANDLE != LOG_HANDLE_INVALID) PRINT_INFO(APP_LOG_HANDLE, fmt, ##args); } while (0)
#define LOG_D(fmt, args...) do { if (APP_LOG_HANDLE != LOG_HANDLE_INVALID) PRINT_DEBUG(APP_LOG_HANDLE, fmt, ##args); } while (0)

/** 模块 init 用：在 mgr 下注册模块名并返回句柄；mgr 无效时返回无效句柄。 */
static inline log_handle_t app_log_register_module(log_mgr_t mgr, const char *module)
{
    if (mgr == LOG_MGR_INVALID || !module)
        return LOG_HANDLE_INVALID;
    return log_register(mgr, module);
}

#endif /* APP_LOG_H */
