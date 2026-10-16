#ifndef APP_LOG_H
#define APP_LOG_H

#include "log_manager_pro.h"

/*
 * 进程级日志句柄：由 main 在 log_manager_init/log_register 成功后填充，
 * 其余模块统一经 LOG_* 宏输出，无需各自持有句柄。
 * 初始化失败时 g_log 保持 LOG_HANDLE_INVALID，LOG_* 自动降为无操作。
 */
extern log_handle_t g_log;

#define LOG_E(fmt, args...) do { if (g_log != LOG_HANDLE_INVALID) PRINT_ERROR(g_log, fmt, ##args); } while (0)
#define LOG_W(fmt, args...) do { if (g_log != LOG_HANDLE_INVALID) PRINT_WARN(g_log, fmt, ##args); } while (0)
#define LOG_I(fmt, args...) do { if (g_log != LOG_HANDLE_INVALID) PRINT_INFO(g_log, fmt, ##args); } while (0)
#define LOG_D(fmt, args...) do { if (g_log != LOG_HANDLE_INVALID) PRINT_DEBUG(g_log, fmt, ##args); } while (0)

#endif /* APP_LOG_H */
