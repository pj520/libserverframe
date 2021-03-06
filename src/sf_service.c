//sf_service.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "sf_global.h"
#include "sf_nio.h"
#include "sf_service.h"

int g_worker_thread_count = 0;
int g_server_outer_sock = -1;
int g_server_inner_sock = -1;
static sf_accept_done_callback sf_accept_done_func = NULL;

static bool bTerminateFlag = false;

static void sigQuitHandler(int sig);
static void sigHupHandler(int sig);
static void sigUsrHandler(int sig);

#if defined(DEBUG_FLAG)
static void sigDumpHandler(int sig);
#endif


static void *worker_thread_entrance(void* arg);

int sf_service_init(sf_alloc_thread_extra_data_callback
        alloc_thread_extra_data_callback,
        ThreadLoopCallback thread_loop_callback,
        sf_accept_done_callback accept_done_callback,
        sf_set_body_length_callback set_body_length_func,
        sf_deal_task_func deal_func, TaskCleanUpCallback task_cleanup_func,
        sf_recv_timeout_callback timeout_callback, const int net_timeout_ms,
        const int proto_header_size, const int task_arg_size)
{
#define ALLOC_CONNECTIONS_ONCE 1024
    int result;
    int bytes;
    int m;
    int init_connections;
    int alloc_conn_once;
    struct nio_thread_data *pThreadData;
    struct nio_thread_data *pDataEnd;
    pthread_t tid;
    pthread_attr_t thread_attr;

    sf_accept_done_func = accept_done_callback;
    sf_set_parameters(proto_header_size, set_body_length_func, deal_func,
        task_cleanup_func, timeout_callback);

    if ((result=set_rand_seed()) != 0) {
        logCrit("file: "__FILE__", line: %d, "
            "set_rand_seed fail, program exit!", __LINE__);
        return result;
    }

    if ((result=init_pthread_attr(&thread_attr, g_sf_global_vars.thread_stack_size)) != 0) {
        logError("file: "__FILE__", line: %d, "
            "init_pthread_attr fail, program exit!", __LINE__);
        return result;
    }

    m = g_sf_global_vars.min_buff_size / (64 * 1024);
    if (m == 0) {
        m = 1;
    } else if (m > 16) {
        m = 16;
    }
    alloc_conn_once = ALLOC_CONNECTIONS_ONCE / m;
    init_connections = g_sf_global_vars.max_connections < alloc_conn_once ?
        g_sf_global_vars.max_connections : alloc_conn_once;
    if ((result=free_queue_init_ex(g_sf_global_vars.max_connections, init_connections,
                    alloc_conn_once, g_sf_global_vars.min_buff_size, g_sf_global_vars.max_buff_size,
                    task_arg_size)) != 0)
    {
        return result;
    }

    bytes = sizeof(struct nio_thread_data) * g_sf_global_vars.work_threads;
    g_sf_global_vars.thread_data = (struct nio_thread_data *)malloc(bytes);
    if (g_sf_global_vars.thread_data == NULL) {
        logError("file: "__FILE__", line: %d, "
            "malloc %d bytes fail, errno: %d, error info: %s",
            __LINE__, bytes, errno, strerror(errno));
        return errno != 0 ? errno : ENOMEM;
    }
    memset(g_sf_global_vars.thread_data, 0, bytes);

    g_worker_thread_count = 0;
    pDataEnd = g_sf_global_vars.thread_data + g_sf_global_vars.work_threads;
    for (pThreadData=g_sf_global_vars.thread_data; pThreadData<pDataEnd; pThreadData++) {
        pThreadData->thread_loop_callback = thread_loop_callback;
        if (alloc_thread_extra_data_callback != NULL) {
            pThreadData->arg = alloc_thread_extra_data_callback(
                    (int)(pThreadData - g_sf_global_vars.thread_data));
        }
        else {
            pThreadData->arg = NULL;
        }

        if (ioevent_init(&pThreadData->ev_puller,
            g_sf_global_vars.max_connections + 2, net_timeout_ms, 0) != 0)
        {
            result  = errno != 0 ? errno : ENOMEM;
            logError("file: "__FILE__", line: %d, "
                "ioevent_init fail, "
                "errno: %d, error info: %s",
                __LINE__, result, strerror(result));
            return result;
        }

        result = fast_timer_init(&pThreadData->timer,
                2 * g_sf_global_vars.network_timeout, g_current_time);
        if (result != 0) {
            logError("file: "__FILE__", line: %d, "
                "fast_timer_init fail, "
                "errno: %d, error info: %s",
                __LINE__, result, strerror(result));
            return result;
        }

        if (pipe(pThreadData->pipe_fds) != 0) {
            result = errno != 0 ? errno : EPERM;
            logError("file: "__FILE__", line: %d, "
                "call pipe fail, "
                "errno: %d, error info: %s",
                __LINE__, result, strerror(result));
            break;
        }

#if defined(OS_LINUX)
        if ((result=fd_add_flags(pThreadData->pipe_fds[0],
                O_NONBLOCK | O_NOATIME)) != 0)
        {
            break;
        }
#else
        if ((result=fd_add_flags(pThreadData->pipe_fds[0],
                O_NONBLOCK)) != 0)
        {
            break;
        }
#endif

        if ((result=pthread_create(&tid, &thread_attr,
            worker_thread_entrance, pThreadData)) != 0)
        {
            logError("file: "__FILE__", line: %d, "
                "create thread failed, startup threads: %d, "
                "errno: %d, error info: %s",
                __LINE__, g_worker_thread_count,
                result, strerror(result));
            break;
        }
        else {
            __sync_fetch_and_add(&g_worker_thread_count, 1);
        }
    }

    pthread_attr_destroy(&thread_attr);

    return 0;
}

int sf_service_destroy()
{
    struct nio_thread_data *pDataEnd, *pThreadData;

    free_queue_destroy();
    pDataEnd = g_sf_global_vars.thread_data + g_sf_global_vars.work_threads;
    for (pThreadData=g_sf_global_vars.thread_data; pThreadData<pDataEnd; pThreadData++) {
        fast_timer_destroy(&pThreadData->timer);
    }
    free(g_sf_global_vars.thread_data);
    g_sf_global_vars.thread_data = NULL;
    return 0;
}

static void *worker_thread_entrance(void* arg)
{
    struct nio_thread_data *pThreadData;

    pThreadData = (struct nio_thread_data *)arg;
    ioevent_loop(pThreadData, sf_recv_notify_read, sf_get_task_cleanup_func(),
        &g_sf_global_vars.continue_flag);
    ioevent_destroy(&pThreadData->ev_puller);

    __sync_fetch_and_sub(&g_worker_thread_count, 1);
    return NULL;
}

static int _socket_server(const char *bind_addr, int port, int *sock)
{
    int result;
    *sock = socketServer(bind_addr, port, &result);
    if (*sock < 0) {
        return result;
    }

    if ((result=tcpsetserveropt(*sock, g_sf_global_vars.network_timeout)) != 0) {
        return result;
    }

    return 0;
}

int sf_socket_server()
{
    int result;
    if (g_sf_global_vars.outer_port != g_sf_global_vars.inner_port) {
        if ((result=_socket_server(g_sf_global_vars.outer_bind_addr, g_sf_global_vars.outer_port,
                        &g_server_outer_sock)) != 0)
        {
            return result;
        }

        if ((result=_socket_server(g_sf_global_vars.inner_bind_addr, g_sf_global_vars.inner_port,
                        &g_server_inner_sock)) != 0)
        {
            return result;
        }
    } else {
        const char *bind_addr;
        if (*g_sf_global_vars.outer_bind_addr != '\0') {
            if (*g_sf_global_vars.inner_bind_addr != '\0') {
                bind_addr = "";
            } else {
                bind_addr = g_sf_global_vars.outer_bind_addr;
            }
        } else {
            bind_addr = g_sf_global_vars.inner_bind_addr;
        }

        if ((result=_socket_server(bind_addr, g_sf_global_vars.outer_port,
                        &g_server_outer_sock)) != 0)
        {
            return result;
        }
    }

    return 0;
}

static void *accept_thread_entrance(void* arg)
{
    int server_sock;
    int incomesock;
    long task_ptr;
    struct sockaddr_in inaddr;
    socklen_t sockaddr_len;
    struct fast_task_info *pTask;
    char szClientIp[IP_ADDRESS_SIZE];

    server_sock = (long)arg;
    while (g_sf_global_vars.continue_flag) {
        sockaddr_len = sizeof(inaddr);
        incomesock = accept(server_sock, (struct sockaddr*)&inaddr, &sockaddr_len);
        if (incomesock < 0) { //error
            if (!(errno == EINTR || errno == EAGAIN)) {
                logError("file: "__FILE__", line: %d, "
                    "accept fail, errno: %d, error info: %s",
                    __LINE__, errno, strerror(errno));
            }

            continue;
        }

        getPeerIpaddr(incomesock,
                szClientIp, IP_ADDRESS_SIZE);
        if (tcpsetnonblockopt(incomesock) != 0) {
            close(incomesock);
            continue;
        }

        pTask = free_queue_pop();
        if (pTask == NULL) {
            logError("file: "__FILE__", line: %d, "
                "malloc task buff failed, you should "
                "increase the parameter: max_connections",
                __LINE__);
            close(incomesock);
            continue;
        }
        strcpy(pTask->client_ip, szClientIp);

        pTask->event.fd = incomesock;
        pTask->thread_data = g_sf_global_vars.thread_data + incomesock % g_sf_global_vars.work_threads;
        if (sf_accept_done_func != NULL) {
            sf_accept_done_func(pTask, server_sock == g_server_inner_sock);
        }

        task_ptr = (long)pTask;
        if (write(pTask->thread_data->pipe_fds[1], &task_ptr,
            sizeof(task_ptr)) != sizeof(task_ptr))
        {
            logError("file: "__FILE__", line: %d, "
                "call write to pipe fd: %d fail, "
                "errno: %d, error info: %s",
                __LINE__, pTask->thread_data->pipe_fds[1],
                errno, strerror(errno));
            close(incomesock);
            free_queue_push(pTask);
        }
    }

    return NULL;
}

void _accept_loop(int server_sock, const int accept_threads)
{
    pthread_t tid;
    pthread_attr_t thread_attr;
    int result;
    int i;

    if ((result=init_pthread_attr(&thread_attr, g_sf_global_vars.thread_stack_size)) != 0) {
        logWarning("file: "__FILE__", line: %d, "
                "init_pthread_attr fail!", __LINE__);
    }
    else {
        for (i=0; i<accept_threads; i++) {
            if ((result=pthread_create(&tid, &thread_attr,
                            accept_thread_entrance,
                            (void *)(long)server_sock)) != 0)
            {
                logError("file: "__FILE__", line: %d, "
                        "create thread failed, startup threads: %d, "
                        "errno: %d, error info: %s",
                        __LINE__, i, result, strerror(result));
                break;
            }
        }

        pthread_attr_destroy(&thread_attr);
    }
}

void sf_accept_loop()
{
    if (g_sf_global_vars.outer_port != g_sf_global_vars.inner_port) {
        _accept_loop(g_server_inner_sock, g_sf_global_vars.accept_threads);
    }

    _accept_loop(g_server_outer_sock, g_sf_global_vars.accept_threads - 1);
    accept_thread_entrance((void *)(long)g_server_outer_sock);
}

#if defined(DEBUG_FLAG)
static void sigDumpHandler(int sig)
{
    static bool bDumpFlag = false;
    char filename[256];

    if (bDumpFlag) {
        return;
    }

    bDumpFlag = true;

    snprintf(filename, sizeof(filename), 
        "%s/logs/sf_dump.log", g_sf_global_vars.base_path);
    //manager_dump_global_vars_to_file(filename);

    bDumpFlag = false;
}
#endif

static void sigQuitHandler(int sig)
{
    if (!bTerminateFlag) {
        bTerminateFlag = true;
        g_sf_global_vars.continue_flag = false;
        logCrit("file: "__FILE__", line: %d, " \
            "catch signal %d, program exiting...", \
            __LINE__, sig);
    }
}

static void sigHupHandler(int sig)
{
    logInfo("file: "__FILE__", line: %d, " \
        "catch signal %d", __LINE__, sig);
}

static void sigUsrHandler(int sig)
{
    logInfo("file: "__FILE__", line: %d, "
        "catch signal %d, ignore it", __LINE__, sig);
}

int sf_setup_signal_handler()
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);

    act.sa_handler = sigUsrHandler;
    if(sigaction(SIGUSR1, &act, NULL) < 0 ||
        sigaction(SIGUSR2, &act, NULL) < 0)
    {
        logCrit("file: "__FILE__", line: %d, "
            "call sigaction fail, errno: %d, error info: %s",
            __LINE__, errno, strerror(errno));
        logCrit("exit abnormally!\n");
        return errno;
    }

    act.sa_handler = sigHupHandler;
    if(sigaction(SIGHUP, &act, NULL) < 0) {
        logCrit("file: "__FILE__", line: %d, "
            "call sigaction fail, errno: %d, error info: %s",
            __LINE__, errno, strerror(errno));
        logCrit("exit abnormally!\n");
        return errno;
    }
    
    act.sa_handler = SIG_IGN;
    if(sigaction(SIGPIPE, &act, NULL) < 0) {
        logCrit("file: "__FILE__", line: %d, "
            "call sigaction fail, errno: %d, error info: %s",
            __LINE__, errno, strerror(errno));
        logCrit("exit abnormally!\n");
        return errno;
    }

    act.sa_handler = sigQuitHandler;
    if(sigaction(SIGINT, &act, NULL) < 0 ||
        sigaction(SIGTERM, &act, NULL) < 0 ||
        sigaction(SIGQUIT, &act, NULL) < 0)
    {
        logCrit("file: "__FILE__", line: %d, "
            "call sigaction fail, errno: %d, error info: %s",
            __LINE__, errno, strerror(errno));
        logCrit("exit abnormally!\n");
        return errno;
    }

#if defined(DEBUG_FLAG)
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_handler = sigDumpHandler;
    if(sigaction(SIGUSR1, &act, NULL) < 0 ||
        sigaction(SIGUSR2, &act, NULL) < 0)
    {
        logCrit("file: "__FILE__", line: %d, "
            "call sigaction fail, errno: %d, error info: %s",
            __LINE__, errno, strerror(errno));
        logCrit("exit abnormally!\n");
        return errno;
    }
#endif
    return 0;
}

int sf_startup_schedule(pthread_t *schedule_tid)
{
#define SCHEDULE_ENTRIES_COUNT 3

    ScheduleArray scheduleArray;
    ScheduleEntry scheduleEntries[SCHEDULE_ENTRIES_COUNT];
    int index;

    scheduleArray.entries = scheduleEntries;
    scheduleArray.count = 0;

    memset(scheduleEntries, 0, sizeof(scheduleEntries));

    index = scheduleArray.count++;
    INIT_SCHEDULE_ENTRY(scheduleEntries[index], sched_generate_next_id(),
            TIME_NONE, TIME_NONE, 0,
             g_sf_global_vars.sync_log_buff_interval,
             log_sync_func, &g_log_context);

    if (g_sf_global_vars.rotate_error_log) {
        log_set_rotate_time_format(&g_log_context, "%Y%m%d");

        index = scheduleArray.count++;
        INIT_SCHEDULE_ENTRY(scheduleEntries[index], sched_generate_next_id(),
                0, 0, 0, 86400, log_notify_rotate, &g_log_context);

        if (g_sf_global_vars.log_file_keep_days > 0) {
            log_set_keep_days(&g_log_context,
                    g_sf_global_vars.log_file_keep_days);

            index = scheduleArray.count++;
            INIT_SCHEDULE_ENTRY(scheduleEntries[index], sched_generate_next_id(),
                    1, 0, 0, 86400, log_delete_old_files, &g_log_context);
        }
    }

    return sched_start(&scheduleArray, schedule_tid,
            g_sf_global_vars.thread_stack_size, (bool * volatile)
            &g_sf_global_vars.continue_flag);
}

void sf_set_current_time()
{
    g_current_time = time(NULL);
    g_sf_global_vars.up_time = g_current_time;
    srand(g_sf_global_vars.up_time);
}
