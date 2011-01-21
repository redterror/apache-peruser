/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2003 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/* Peruser version 0.4.0 */

/* #define MPM_PERUSER_DEBUG */

#include "apr.h"
#include "apr_hash.h"
#include "apr_pools.h"
#include "apr_file_io.h"
#include "apr_portable.h"
#include "apr_strings.h"
#include "apr_thread_proc.h"
#include "apr_signal.h"
#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#define APR_WANT_IOVEC
#include "apr_want.h"

#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif
#if APR_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#define CORE_PRIVATE

#include "ap_config.h"
#include "httpd.h"
#include "mpm_default.h"
#include "http_main.h"
#include "http_log.h"
#include "http_config.h"
#include "http_core.h"		/* for get_remote_host */
#include "http_connection.h"
#include "http_protocol.h"	/* for ap_hook_post_read_request */
#include "http_vhost.h"		/* for ap_update_vhost_given_ip */
#include "scoreboard.h"
#include "ap_mpm.h"
#include "unixd.h"
#include "mpm_common.h"
#include "ap_listen.h"
#include "ap_mmn.h"
#include "apr_poll.h"
#include "util_ebcdic.h"
#include "mod_status.h"

#ifdef HAVE_BSTRING_H
#include <bstring.h>		/* for IRIX, FD_SET calls bzero() */
#endif

#ifdef HAVE_TIME_H
#include <time.h>
#endif

#ifdef HAVE_SYS_PROCESSOR_H
#include <sys/processor.h> /* for bindprocessor() */
#endif

#if APR_HAS_SHARED_MEMORY
#include "apr_shm.h"
#else
#error "Peruser MPM requres shared memory support."
#endif

/* should be APR-ized */
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <setjmp.h>

#include <signal.h>
#include <sys/times.h>

#include "cpu_usage.h"

#ifdef MPM_PERUSER_DEBUG
# define _DBG(text,par...) \
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, \
                 "(peruser: pid=%d uid=%d child=%d) %s(): " text, \
                 getpid(), getuid(), my_child_num, __FUNCTION__, ##par, 0)

# define _TRACE_CALL(text,par...) _DBG("calling " text, ##par)
# define _TRACE_RET(text,par...) _DBG("returned from " text, ##par)
#else
# define _DBG(text,par...)
# define _TRACE_RET(text,par...)
# define _TRACE_CALL(text,par...)
#endif /* MPM_PERUSER_DEBUG */

/* char of death - for signalling children to die */
#define AP_PERUSER_CHAR_OF_DEATH        '!'

#define PERUSER_SERVER_CONF(cf)        \
    ((peruser_server_conf *) ap_get_module_config(cf, &mpm_peruser_module))

#define SCOREBOARD_STATUS(i)	ap_scoreboard_image->servers[i][0].status

/*
 * Define some magic numbers that we use for the state of the incomming
 * request. These must be < 0 so they don't collide with a file descriptor.
 */
#define AP_PERUSER_THISCHILD -1
#define AP_PERUSER_OTHERCHILD -2

/* Limit on the total --- clients will be locked out if more servers than
 * this are needed.  It is intended solely to keep the server from crashing
 * when things get out of hand.
 *
 * We keep a hard maximum number of servers, for two reasons --- first off,
 * in case something goes seriously wrong, we want to stop the fork bomb
 * short of actually crashing the machine we're running on by filling some
 * kernel table.  Secondly, it keeps the size of the scoreboard file small
 * enough that we can read the whole thing without worrying too much about
 * the overhead.
 */
#ifndef DEFAULT_SERVER_LIMIT
#define DEFAULT_SERVER_LIMIT 256
#endif

/* Admin can't tune ServerLimit beyond MAX_SERVER_LIMIT.  We want
 * some sort of compile-time limit to help catch typos.
 */
#ifndef MAX_SERVER_LIMIT
#define MAX_SERVER_LIMIT 20000
#endif

#ifndef HARD_THREAD_LIMIT
#define HARD_THREAD_LIMIT 1
#endif

#define CHILD_TYPE_RESERVED    -1
#define CHILD_TYPE_UNKNOWN      0
#define CHILD_TYPE_MULTIPLEXER  1
#define CHILD_TYPE_PROCESSOR    2
#define CHILD_TYPE_WORKER       3

#define CHILD_STATUS_STANDBY  0  /* wait for a request before starting */
#define CHILD_STATUS_STARTING 1  /* wait for socket creation */
#define CHILD_STATUS_READY    2  /* is ready to take requests */
#define CHILD_STATUS_ACTIVE   3  /* is currently busy handling requests */
#define CHILD_STATUS_RESTART  4  /* child about to die and restart */

/* cgroup settings */
#define CGROUP_TASKS_FILE "/tasks"
#define CGROUP_TASKS_FILE_LEN 7

/* config globals */

int ap_threads_per_child = 0; /* Worker threads per child */
static apr_proc_mutex_t *accept_mutex;
static int ap_start_processors = DEFAULT_START_PROCESSORS;
static int ap_min_processors = DEFAULT_MIN_PROCESSORS;
static int ap_min_free_processors = DEFAULT_MIN_FREE_PROCESSORS;
static int ap_max_free_processors = DEFAULT_MAX_FREE_PROCESSORS;
static int ap_max_processors = DEFAULT_MAX_PROCESSORS;
static int ap_max_cpu_usage = DEFAULT_MAX_CPU_USAGE;
static int ap_min_multiplexers = DEFAULT_MIN_MULTIPLEXERS;
static int ap_max_multiplexers = DEFAULT_MAX_MULTIPLEXERS;
static int ap_daemons_limit = 0; /* MaxClients */
static int expire_timeout = DEFAULT_EXPIRE_TIMEOUT;
static int idle_timeout = DEFAULT_IDLE_TIMEOUT;
static int multiplexer_idle_timeout = DEFAULT_MULTIPLEXER_IDLE_TIMEOUT;
static int processor_wait_timeout = DEFAULT_PROCESSOR_WAIT_TIMEOUT;
static int processor_wait_steps = DEFAULT_PROCESSOR_WAIT_STEPS;
static int server_limit = DEFAULT_SERVER_LIMIT;
static int first_server_limit;
static int changed_limit_at_restart;
static int requests_this_child;
static int mpm_state = AP_MPMQ_STARTING;
static ap_pod_t *pod;

/* === configuration stuff === */

typedef struct
{
    int id;
    int processor_id;

    const char *name; /* Server environment's unique string identifier */

    /* security settings */
    uid_t uid; /* user id */
    gid_t gid; /* group id */
    const char *chroot; /* directory to chroot() to, can be null */
    short nice_lvl;
    const char *cgroup; /* cgroup directory, can be null */

    /* resource settings */
    int start_processors;
    int min_processors;
    int min_free_processors;
    int max_free_processors;
    int max_processors;
    int max_cpu_usage;
    short availability;

    /* sockets */
    int input;  /* The socket descriptor */
    int output; /* The socket descriptor */

    /* error flags */
    /* we use these to reduce log clutter (report only on first failure) */
    short error_cgroup; /* When writing pid to cgroup fails */
    short error_pass;   /* When unable to pass request to the processor */

    /* statistics */
    unsigned long stats_requests;    /* requests handled */
    unsigned long stats_connections; /* connections handled */
    unsigned long stats_dropped;     /* connections dropped because multiplexer was not able to pass */

    /* counters */
    int total_processors;
    int idle_processors;
    int active_processors;
    int total_processes;

    /* tmp counters */
    int tmp_total_processors;
    int tmp_idle_processors;
    int tmp_active_processors;
    int tmp_total_processes;

    /* cpu usage */
    double tmp_total_cpu_usage;
    double total_cpu_usage;
} server_env_t;

typedef struct
{
    apr_size_t num;
} server_env_control;

typedef struct
{
    server_env_control *control;
    server_env_t *table;
} server_env;

typedef struct
{
    /* identification */
    int id;             /* index in child_info_table */
    pid_t pid;          /* process id */
    int status;         /* status of child */
    int type;           /* multiplexer or processor */
    server_env_t *senv;

    /* sockets */
    int sock_fd;

    /* stack context saved state */
    jmp_buf jmpbuffer;

    int cpu_stopped;
    double cpu_usage;
} child_info_t;

typedef struct
{
    apr_size_t num;
    int new;
} child_info_control;

typedef struct
{
    child_info_control *control;
    child_info_t *table;
} child_info;

typedef struct
{
    server_env_t *senv;
    short missing_senv_reported;
} peruser_server_conf;

typedef struct peruser_header
{
    char *headers;
    apr_pool_t *p;
} peruser_header;

/* Tables used to determine the user and group each child process should
 * run as.  The hash table is used to correlate a server name with a child
 * process.
 */
static apr_size_t child_info_size;
static child_info *child_info_image = NULL;

#define NUM_CHILDS (child_info_image != NULL ? child_info_image->control->num : 0)
#define CHILD_INFO_TABLE (child_info_image != NULL ? child_info_image->table : NULL)

static apr_size_t server_env_size;
static server_env *server_env_image = NULL;

#define NUM_SENV (server_env_image != NULL ? server_env_image->control->num : 0)
#define SENV (server_env_image != NULL ? server_env_image->table : NULL)

#if APR_HAS_SHARED_MEMORY
#ifndef WIN32
static /* but must be exported to mpm_winnt */
#endif
apr_shm_t *child_info_shm = NULL;
apr_shm_t *server_env_shm = NULL;
#endif

server_rec *ap_server_conf;

module AP_MODULE_DECLARE_DATA mpm_peruser_module;

/* -- replace the pipe-of-death by an control socket -- */
static apr_file_t *pipe_of_death_in = NULL;
static apr_file_t *pipe_of_death_out = NULL;

/* one_process --- debugging mode variable; can be set from the command line
 * with the -X flag.  If set, this gets you the child_main loop running
 * in the process which originally started up (no detach, no make_child),
 * which is a pretty nice debugging environment.  (You'll get a SIGHUP
 * early in standalone_main; just continue through.  This is the server
 * trying to kill off any child processes which it might have lying
 * around --- Apache doesn't keep track of their pids, it just sends
 * SIGHUP to the process group, ignoring it in the root process.
 * Continue through and you'll be fine.).
 */

static int one_process = 0;

static apr_pool_t *pconf; /* Pool for config stuff */
static apr_pool_t *pchild; /* Pool for httpd child stuff */

static pid_t ap_my_pid; /* it seems silly to call getpid all the time */
static pid_t parent_pid;
static int my_child_num;
ap_generation_t volatile ap_my_generation = 0;

#ifdef TPF
int tpf_child = 0;
char tpf_server_name[INETD_SERVNAME_LENGTH+1];
#endif /* TPF */

static int die_now = 0;

int server_env_cleanup = 1;
const char *multiplexer_chroot = NULL;
server_env_t *multiplexer_senv;

/* function added to mod_ssl and exported (there was nothing useful for us) */
typedef int (*ssl_server_is_https_t)(server_rec*);
ssl_server_is_https_t ssl_server_is_https = NULL;

#ifdef GPROF
/* 
 * change directory for gprof to plop the gmon.out file
 * configure in httpd.conf:
 * GprofDir $RuntimeDir/   -> $ServerRoot/$RuntimeDir/gmon.out
 * GprofDir $RuntimeDir/%  -> $ServerRoot/$RuntimeDir/gprof.$pid/gmon.out
 */
static void chdir_for_gprof(void)
{
    core_server_config *sconf =
    ap_get_module_config(ap_server_conf->module_config, &core_module);
    char *dir = sconf->gprof_dir;
    const char *use_dir;

    if(dir) {
        apr_status_t res;
        char buf[512];
        int len = strlen(sconf->gprof_dir) - 1;
        if(*(dir + len) == '%') {
            dir[len] = '\0';
            apr_snprintf(buf, sizeof(buf), "%sgprof.%d", dir, (int)getpid());
        }
        use_dir = ap_server_root_relative(pconf, buf[0] ? buf : dir);
        res = apr_dir_make(use_dir, 0755, pconf);
        if(res != APR_SUCCESS && !APR_STATUS_IS_EEXIST(res)) {
            ap_log_error(APLOG_MARK, APLOG_ERR, errno, ap_server_conf,
                    "gprof: error creating directory %s", dir);
        }
    }
    else {
        use_dir = ap_server_root_relative(pconf, DEFAULT_REL_RUNTIMEDIR);
    }

    chdir(use_dir);
}
#else
#define chdir_for_gprof()
#endif

char* child_type_string(int type)
{
    switch (type) {
    case CHILD_TYPE_MULTIPLEXER:
        return "MULTIPLEXER";
    case CHILD_TYPE_PROCESSOR:
        return "PROCESSOR";
    case CHILD_TYPE_WORKER:
        return "WORKER";
    case CHILD_TYPE_RESERVED:
        return "RESERVED";
    }

    return "UNKNOWN";
}

char* child_status_string(int status)
{
    switch (status) {
    case CHILD_STATUS_STANDBY:
        return "STANDBY";
    case CHILD_STATUS_STARTING:
        return "STARTING";
    case CHILD_STATUS_READY:
        return "READY";
    case CHILD_STATUS_ACTIVE:
        return "ACTIVE";
    case CHILD_STATUS_RESTART:
        return "RESTART";
    }

    return "UNKNOWN";
}

char* scoreboard_status_string(int status)
{
    switch (status) {
    case SERVER_DEAD:
        return "DEAD";
    case SERVER_STARTING:
        return "STARTING";
    case SERVER_READY:
        return "READY";
    case SERVER_BUSY_READ:
        return "BUSY_READ";
    case SERVER_BUSY_WRITE:
        return "BUSY_WRITE";
    case SERVER_BUSY_KEEPALIVE:
        return "BUSY_KEEPALIVE";
    case SERVER_BUSY_LOG:
        return "BUSY_LOG";
    case SERVER_BUSY_DNS:
        return "BUSY_DNS";
    case SERVER_CLOSING:
        return "CLOSING";
    case SERVER_GRACEFUL:
        return "GRACEFUL";
    case SERVER_NUM_STATUS:
        return "NUM_STATUS";
    }

    return "UNKNOWN";
}

void dump_child_table()
{
#ifdef MPM_PERUSER_DEBUG
    int x;
    server_env_t *senv;

    _DBG("%-3s %-5s %-8s %-12s %-4s %-4s %-25s %5s %6s %7s",
         "ID", "PID", "STATUS", "TYPE", "UID", "GID", "CHROOT", "INPUT",
         "OUTPUT", "SOCK_FD");

    for(x = 0; x < NUM_CHILDS; x++)
    {
        senv = CHILD_INFO_TABLE[x].senv;
        _DBG("%-3d %-5d %-8s %-12s %-4d %-4d %-25s %-5d %-6d %-7d",
                CHILD_INFO_TABLE[x].id,
                CHILD_INFO_TABLE[x].pid,
                child_status_string(CHILD_INFO_TABLE[x].status),
                child_type_string(CHILD_INFO_TABLE[x].type),
                senv == NULL ? -1 : senv->uid,
                senv == NULL ? -1 : senv->gid,
                senv == NULL ? NULL : senv->chroot,
                senv == NULL ? -1 : CHILD_INFO_TABLE[x].senv->input,
                senv == NULL ? -1 : CHILD_INFO_TABLE[x].senv->output,
                CHILD_INFO_TABLE[x].sock_fd);
    }
#endif
}

void dump_server_env_image()
{
#ifdef MPM_PERUSER_DEBUG
    int x;
    _DBG("%-3s %-7s %-7s %-7s", "N", "INPUT", "OUTPUT", "CHROOT");
    for(x = 0; x < NUM_SENV; x++)
    {
        _DBG("%-3d %-7d %-7d %-7s", x, SENV[x].input, SENV[x].output,
             SENV[x].chroot);
    }
#endif
}

/* XXX - I don't know if TPF will ever use this module or not, so leave
 * the ap_check_signals calls in but disable them - manoj */
#define ap_check_signals() 

/* a clean exit from a child with proper cleanup */
static inline int clean_child_exit(int code) __attribute__ ((noreturn));
static inline int clean_child_exit(int code)
{
    int retval;

    mpm_state = AP_MPMQ_STOPPING;

    if (CHILD_INFO_TABLE[my_child_num].type != CHILD_TYPE_MULTIPLEXER
            && CHILD_INFO_TABLE[my_child_num].senv)
    {
        retval = close(CHILD_INFO_TABLE[my_child_num].senv->input);
        _DBG("close(CHILD_INFO_TABLE[%d].senv->input) = %d",
                my_child_num, retval);

        retval = close(CHILD_INFO_TABLE[my_child_num].senv->output);
        _DBG("close(CHILD_INFO_TABLE[%d].senv->output) = %d",
                my_child_num, retval);
    }

    if (pchild) {
        apr_pool_destroy(pchild);
    }
    ap_mpm_pod_close(pod);
    chdir_for_gprof();
    exit(code);
}

/* number of calls to wait_or_timeout between writable probes */
#ifndef INTERVAL_OF_WRITABLE_PROBES
#define INTERVAL_OF_WRITABLE_PROBES 10
#endif
static int wait_or_timeout_counter;
  
void ap_wait_or_timeout(apr_exit_why_e *status, int *exitcode, apr_proc_t *ret,
                        apr_pool_t *p)
{
    apr_status_t rv;

    ++wait_or_timeout_counter;
    if (wait_or_timeout_counter == INTERVAL_OF_WRITABLE_PROBES) {
        wait_or_timeout_counter = 0;
        ap_run_monitor(p);
    }

    rv = apr_proc_wait_all_procs(ret, exitcode, status, APR_NOWAIT, p);
    if (APR_STATUS_IS_EINTR(rv)) {
        ret->pid = -1;
        return;
    }

    if (APR_STATUS_IS_CHILD_DONE(rv)) {
        return;
    }

#ifdef NEED_WAITPID
    if ((ret = reap_children(exitcode, status)) > 0) {
        return;
    }
#endif

    ret->pid = -1;
    return;
}

static void accept_mutex_on(void)
{
    apr_status_t rv = apr_proc_mutex_lock(accept_mutex);
    if (rv != APR_SUCCESS) {
        const char *msg = "couldn't grab the accept mutex";

        if (ap_my_generation != ap_scoreboard_image->global->running_generation)
        {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, NULL, msg);
            clean_child_exit(0);
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, NULL, msg);
            exit(APEXIT_CHILDFATAL);
        }
    }
}

static void accept_mutex_off(void)
{
    apr_status_t rv = apr_proc_mutex_unlock(accept_mutex);
    if (rv != APR_SUCCESS) {
        const char *msg = "couldn't release the accept mutex";

        if (ap_my_generation != ap_scoreboard_image->global->running_generation)
        {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, NULL, msg);
            /* don't exit here... we have a connection to
             * process, after which point we'll see that the
             * generation changed and we'll exit cleanly
             */
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, NULL, msg);
            exit(APEXIT_CHILDFATAL);
        }
    }
}

/* On some architectures it's safe to do unserialized accept()s in the single
 * Listen case.  But it's never safe to do it in the case where there's
 * multiple Listen statements.  Define SINGLE_LISTEN_UNSERIALIZED_ACCEPT
 * when it's safe in the single Listen case.
 */
#ifdef SINGLE_LISTEN_UNSERIALIZED_ACCEPT
#define SAFE_ACCEPT(stmt) do {if (ap_listeners->next) {stmt;}} while(0)
#else
#define SAFE_ACCEPT(stmt) do {stmt;} while(0)
#endif

AP_DECLARE(apr_status_t) ap_mpm_query(int query_code, int *result)
{
    switch (query_code) {
    case AP_MPMQ_MAX_DAEMON_USED:
        *result = ap_daemons_limit;
        return APR_SUCCESS;
    case AP_MPMQ_IS_THREADED:
        *result = AP_MPMQ_NOT_SUPPORTED;
        return APR_SUCCESS;
    case AP_MPMQ_IS_FORKED:
        *result = AP_MPMQ_DYNAMIC;
        return APR_SUCCESS;
    case AP_MPMQ_HARD_LIMIT_DAEMONS:
        *result = server_limit;
        return APR_SUCCESS;
    case AP_MPMQ_HARD_LIMIT_THREADS:
        *result = HARD_THREAD_LIMIT;
        return APR_SUCCESS;
    case AP_MPMQ_MAX_THREADS:
        *result = 0;
        return APR_SUCCESS;
    case AP_MPMQ_MIN_SPARE_DAEMONS:
        *result = ap_min_free_processors;
        return APR_SUCCESS;
    case AP_MPMQ_MIN_SPARE_THREADS:
        *result = 0;
        return APR_SUCCESS;
    case AP_MPMQ_MAX_SPARE_THREADS:
        *result = 0;
        return APR_SUCCESS;
    case AP_MPMQ_MAX_REQUESTS_DAEMON:
        *result = ap_max_requests_per_child;
        return APR_SUCCESS;
    case AP_MPMQ_MAX_DAEMONS:
        *result = server_limit;
        return APR_SUCCESS;
    case AP_MPMQ_MPM_STATE:
        *result = mpm_state;
        return APR_SUCCESS;
    }
    return APR_ENOTIMPL;
}

#if defined(NEED_WAITPID)
/*
 Systems without a real waitpid sometimes lose a child's exit while waiting
 for another.  Search through the scoreboard for missing children.
 */
int reap_children(int *exitcode, apr_exit_why_e *status)
{
    int n, pid;

    for (n = 0; n < NUM_CHILDS; ++n) {
        if (ap_scoreboard_image->servers[n][0].status != SERVER_DEAD)
            if (unixd_killpg((pid = ap_scoreboard_image->parent[n].pid), 0) == -1) {
                ap_update_child_status_from_indexes(n, 0, SERVER_DEAD, NULL);
                /* just mark it as having a successful exit status */
                *status = APR_PROC_EXIT;
                *exitcode = 0;
                return(pid);
            }
            if (CHILD_INFO_TABLE[n].cpu_stopped) {
                unixd_killpg(ap_scoreboard_image->parent[n].pid, SIGCONT);
                CHILD_INFO_TABLE[n].cpu_stopped = 0;
            }
            CHILD_INFO_TABLE[n].cpu_usage = 0;
        }
    }
    return 0;
}
#endif

/* handle all varieties of core dumping signals */
static void sig_coredump(int sig)
{
    int retval;
    retval = chdir(ap_coredump_dir);
    apr_signal(sig, SIG_DFL);
    if (ap_my_pid == parent_pid) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                     "seg fault or similar nasty error detected "
                         "in the parent process");
    }
    unixd_killpg(getpid(), sig);
    if (CHILD_INFO_TABLE[my_child_num].cpu_stopped) {
        unixd_killpg(getpid(), SIGCONT);
        CHILD_INFO_TABLE[my_child_num].cpu_stopped = 0;
    }
    CHILD_INFO_TABLE[my_child_num].cpu_usage = 0;

    /* At this point we've got sig blocked, because we're still inside
     * the signal handler.  When we leave the signal handler it will
     * be unblocked, and we'll take the signal... and coredump or whatever
     * is appropriate for this particular Unix.  In addition the parent
     * will see the real signal we received -- whereas if we called
     * abort() here, the parent would only see SIGABRT.
     */
}

/*****************************************************************
 * Connection structures and accounting...
 */

static void just_die(int sig)
{
    _DBG("function called");
    clean_child_exit(0);
}

/* volatile just in case */
static int volatile shutdown_pending;
static int volatile restart_pending;
static int volatile is_graceful;
/* XXX static int volatile child_fatal; */

static void sig_term(int sig)
{
    if (shutdown_pending == 1) {
        /* Um, is this _probably_ not an error, if the user has
         * tried to do a shutdown twice quickly, so we won't
         * worry about reporting it.
         */
        return;
    }
    shutdown_pending = 1;
}

/* restart() is the signal handler for SIGHUP and AP_SIG_GRACEFUL
 * in the parent process, unless running in ONE_PROCESS mode
 */
static void restart(int sig)
{
    if (restart_pending == 1) {
        /* Probably not an error - don't bother reporting it */
        return;
    }
    restart_pending = 1;
    is_graceful = (sig == AP_SIG_GRACEFUL);
}

/* Sets die_now if we received a character on the pipe_of_death */
static apr_status_t check_pipe_of_death(void **csd, ap_listen_rec *lr,
        apr_pool_t *ptrans)
{
    int ret;
    char pipe_read_char;
    apr_size_t n = 1;

    _DBG("WATCH: die_now=%d", die_now);

    if (die_now)
        return APR_SUCCESS;

    /* apr_thread_mutex_lock(pipe_of_death_mutex); */
    ret = apr_socket_recv(lr->sd, &pipe_read_char, &n);
    if (APR_STATUS_IS_EAGAIN(ret)) {
        /* It lost the lottery. It must continue to suffer
         * through a life of servitude. */
        _DBG("POD read EAGAIN");
        return ret;
    }
    else {
        if (pipe_read_char != AP_PERUSER_CHAR_OF_DEATH) {
            _DBG("got wrong char %c", pipe_read_char);
            return APR_SUCCESS;
        }
        /* It won the lottery (or something else is very
         * wrong). Embrace death with open arms. */
        die_now = 1;
        _DBG("WATCH: die_now=%d", die_now);
    }
    /* apr_thread_mutex_unlock(pipe_of_death_mutex); */
    return APR_SUCCESS;
}

static void set_signals(void)
{
#ifndef NO_USE_SIGACTION
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (!one_process) {
        sa.sa_handler = sig_coredump;
#if defined(SA_ONESHOT)
        sa.sa_flags = SA_ONESHOT;
#elif defined(SA_RESETHAND)
        sa.sa_flags = SA_RESETHAND;
#endif
        if (sigaction(SIGSEGV, &sa, NULL) < 0)
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "sigaction(SIGSEGV)");
#ifdef SIGBUS
        if (sigaction(SIGBUS, &sa, NULL) < 0)
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "sigaction(SIGBUS)");
#endif
#ifdef SIGABORT
        if (sigaction(SIGABORT, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGABORT)");
#endif
#ifdef SIGABRT
        if (sigaction(SIGABRT, &sa, NULL) < 0)
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "sigaction(SIGABRT)");
#endif
#ifdef SIGILL
        if (sigaction(SIGILL, &sa, NULL) < 0)
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "sigaction(SIGILL)");
#endif
        sa.sa_flags = 0;
    }
    sa.sa_handler = sig_term;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGTERM)");
#ifdef SIGINT
    if (sigaction(SIGINT, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGINT)");
#endif
#ifdef SIGXCPU
    sa.sa_handler = SIG_DFL;
    if (sigaction(SIGXCPU, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGXCPU)");
#endif
#ifdef SIGXFSZ
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGXFSZ, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGXFSZ)");
#endif
#ifdef SIGPIPE
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGPIPE)");
#endif

    /* we want to ignore HUPs and AP_SIG_GRACEFUL while we're busy 
     * processing one */
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaddset(&sa.sa_mask, AP_SIG_GRACEFUL);
    sa.sa_handler = restart;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(SIGHUP)");
    if (sigaction(AP_SIG_GRACEFUL, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                     "sigaction(" AP_SIG_GRACEFUL_STRING ")");
#else
    if (!one_process) {
        apr_signal(SIGSEGV, sig_coredump);
#ifdef SIGBUS
        apr_signal(SIGBUS, sig_coredump);
#endif /* SIGBUS */
#ifdef SIGABORT
        apr_signal(SIGABORT, sig_coredump);
#endif /* SIGABORT */
#ifdef SIGABRT
        apr_signal(SIGABRT, sig_coredump);
#endif /* SIGABRT */
#ifdef SIGILL
        apr_signal(SIGILL, sig_coredump);
#endif /* SIGILL */
#ifdef SIGXCPU
        apr_signal(SIGXCPU, SIG_DFL);
#endif /* SIGXCPU */
#ifdef SIGXFSZ
        apr_signal(SIGXFSZ, SIG_DFL);
#endif /* SIGXFSZ */
    }

    apr_signal(SIGTERM, sig_term);
#ifdef SIGHUP
    apr_signal(SIGHUP, restart);
#endif /* SIGHUP */
#ifdef AP_SIG_GRACEFUL
    apr_signal(AP_SIG_GRACEFUL, restart);
#endif /* AP_SIG_GRACEFUL */
#ifdef SIGPIPE
    apr_signal(SIGPIPE, SIG_IGN);
#endif /* SIGPIPE */

#endif
}

/*****************************************************************
 * Child process main loop.
 * The following vars are static to avoid getting clobbered by longjmp();
 * they are really private to child_main.
 */

static int requests_this_child;
static int num_listensocks = 0;
static ap_listen_rec *listensocks;

int ap_graceful_stop_signalled(void)
{
    /* not ever called anymore... */
    return 0;
}

static int active_env_processors(int env_num)
{
    if (env_num >= NUM_SENV) {
        return -1;
    }
    return SENV[env_num].active_processors;
}

static int update_single_processor_counters(int child_num)
{

    if (child_num >= NUM_CHILDS) return -1;
    if (!CHILD_INFO_TABLE[child_num].senv) return -1;

    int i;
    int tmp_total_processors = 0;
    int tmp_idle_processors = 0;
    int tmp_active_processors = 0;
    int tmp_total_processes = 0;
    double tmp_total_cpu_usage = 0;

    /* counting processors */
    for (i = 0; i < NUM_CHILDS; ++i) {
        if (CHILD_INFO_TABLE[i].senv == CHILD_INFO_TABLE[child_num].senv) {
            tmp_total_cpu_usage += CHILD_INFO_TABLE[i].cpu_usage;
            if (CHILD_INFO_TABLE[i].status != CHILD_STATUS_STANDBY)
                tmp_total_processes++;
            tmp_total_processors++;
            if (CHILD_INFO_TABLE[i].status == CHILD_STATUS_READY
                || CHILD_INFO_TABLE[i].status == CHILD_STATUS_STARTING)
                    tmp_idle_processors++;
            if (CHILD_INFO_TABLE[i].pid > 0)
                tmp_active_processors++;
        }
    }

    /* updating counters */
    CHILD_INFO_TABLE[child_num].senv->total_processors = tmp_total_processors;
    CHILD_INFO_TABLE[child_num].senv->idle_processors = tmp_idle_processors;
    CHILD_INFO_TABLE[child_num].senv->active_processors = tmp_active_processors;
    CHILD_INFO_TABLE[child_num].senv->total_processes = tmp_total_processes;
    CHILD_INFO_TABLE[child_num].senv->total_cpu_usage = tmp_total_cpu_usage;

    return 0;
}

static void update_all_counters()
{
    int i;

    if (!NUM_SENV) return;

    /* resetting counters */
    for (i = 0; i < NUM_SENV; ++i) {
        SENV[i].tmp_total_processors = 0;
        SENV[i].tmp_idle_processors = 0;
        SENV[i].tmp_active_processors = 0;
        SENV[i].tmp_total_processes = 0;
        SENV[i].tmp_total_cpu_usage = 0;        
    }

    /* counting processors */
    for (i = 0; i < NUM_CHILDS; ++i) {
        if (CHILD_INFO_TABLE[i].senv != NULL) {
            CHILD_INFO_TABLE[i].senv->tmp_total_cpu_usage += CHILD_INFO_TABLE[i].cpu_usage;
            CHILD_INFO_TABLE[i].senv->tmp_total_processors++;
            if (CHILD_INFO_TABLE[i].status != CHILD_STATUS_STANDBY)
                CHILD_INFO_TABLE[i].senv->tmp_total_processes++;
            if (CHILD_INFO_TABLE[i].status == CHILD_STATUS_READY
                || CHILD_INFO_TABLE[i].status == CHILD_STATUS_STARTING)
                    CHILD_INFO_TABLE[i].senv->tmp_idle_processors++;
            if (CHILD_INFO_TABLE[i].pid > 0)
                CHILD_INFO_TABLE[i].senv->tmp_active_processors++;
        }
    }

    /* updating counters */
    for (i = 0; i < NUM_SENV; ++i) {
        SENV[i].total_processors = SENV[i].tmp_total_processors;
        SENV[i].idle_processors = SENV[i].tmp_idle_processors;
        SENV[i].active_processors = SENV[i].tmp_active_processors;
        SENV[i].total_processes = SENV[i].tmp_total_processes;
        SENV[i].total_cpu_usage = SENV[i].tmp_total_cpu_usage;
    }

    return;
}

static int wait_for_workers(child_info_t *processor)
{
    int i, wait_step_size, wait_time;

    wait_step_size = 100 / processor_wait_steps;

    /* Protection for graceful restart under high load */
    if (!processor->senv) return -1;
    if (update_single_processor_counters(processor->id) == -1) return -1;

    /* If all processors are busy, we will clone a child */
    if ((processor->senv->idle_processors == 0
         && processor->senv->total_processors > 0
         && processor->senv->total_processors < processor->senv->max_processors
         && processor->senv->total_processors <= processor->senv->total_processes)
        || (processor->senv->idle_processors > processor->senv->total_processors)) {
        _DBG("MULTIPLEXER CLONING CHILD %d", processor->id);
        child_clone(processor->id);
        child_info_image->control->new = 1;
        apr_sleep(apr_time_from_sec(1));
        if (update_single_processor_counters(processor->id) == -1) return -1;
    }

    /*	Check if the processor is available */
    if (processor->senv->total_processors >= processor->senv->max_processors
        && processor->senv->idle_processors == 0 && processor_wait_timeout > 0) {

        /* The processor is currently busy, try to wait (a little) */
        _DBG("processor seems to be busy, trying to wait for it");

        if (processor->senv->availability == 0) {
            processor->senv->availability = 0;

            _DBG("processor is busy (availability = 0) - not passing request");

            if (processor->senv->error_pass == 0) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ap_server_conf,
                             "Too many requests for processor %s, "
                             "increase MaxProcessors", processor->senv->name);
            }

            /* No point in waiting for the processor, it's very busy */
            return -1;
        }

        /* We sleep a little (depending how available the processor is) */
        wait_time = (float) processor_wait_timeout / processor_wait_steps * 1000000;

        for (i = 0; i <= processor->senv->availability; i += wait_step_size) {

            apr_sleep(wait_time);

            /* Check if the processor is ready */
            if (update_single_processor_counters(processor->id) == -1) return -1;
            if (processor->senv->total_processors
                    < processor->senv->max_processors
                    || processor->senv->idle_processors > 0)
            {
                /* The processor has freed - lets use it */
                _DBG("processor freed before wait time expired");
                break;
            }
        }

        if (processor->senv->availability <= wait_step_size) {
            processor->senv->availability = 0;
        }
        else
            processor->senv->availability -= wait_step_size;

        /* Check if we waited all the time */
        if (i > processor->senv->availability) {
            _DBG("processor is busy - not passing request (availability = %d)",
                    processor->senv->availability);

            if (processor->senv->error_pass == 0) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ap_server_conf,
                             "Too many requests for processor %s, "
                             "increase MaxProcessors", processor->senv->name);
            }

            return -1;
        }

        /* We could increase the availability a little here,
         * because the processor got freed eventually
         */
    }
    else {
        /* Smoothly increment the availability back to 100 */
        if (processor->senv->availability >= 100 - wait_step_size) {
            processor->senv->availability = 100;
        } else {
            processor->senv->availability += wait_step_size;
        }
    }

    return 0;
}

/*
 * This function sends a raw socket over to a processor. It uses the same
 * on-wire format as pass_request. The recipient can determine if he got
 * a socket or a whole request by inspecting the header_length of the
 * message. If it is zero then only a socket was sent.
 */
static int pass_socket(apr_socket_t *thesock, child_info_t *processor,
        apr_pool_t *pool)
{
    int rv;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    apr_sockaddr_t *remote_addr;
    int sock_fd;
    char *body = "";
    struct iovec iov[5];
    apr_size_t header_len = 0;
    apr_size_t body_len = 0;
    peruser_header h;

    if (!processor) {
        _DBG("server %s in child %d has no child_info associated",
                "(unknown)", my_child_num);
        return -1;
    }

    /* Make sure there are free workers on the other end */
    if (wait_for_workers(processor) == -1) {
        return -1;
    }

    _DBG("passing request to another child.", 0);

    apr_os_sock_get(&sock_fd, thesock);

    /* passing remote_addr too, see comments below */
    apr_socket_addr_get(&remote_addr, APR_REMOTE, thesock);

    header_len = 0;
    body_len = 0;

    iov[0].iov_base = &header_len;
    iov[0].iov_len = sizeof(header_len);
    iov[1].iov_base = &body_len;
    iov[1].iov_len = sizeof(body_len);
    iov[2].iov_base = remote_addr;
    iov[2].iov_len = sizeof(*remote_addr);
    iov[3].iov_base = h.headers;
    iov[3].iov_len = 0;
    iov[4].iov_base = body;
    iov[4].iov_len = body_len;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 5;

    cmsg = apr_palloc(pool, sizeof(*cmsg) + sizeof(sock_fd));
    cmsg->cmsg_len = CMSG_LEN(sizeof(sock_fd));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    memcpy(CMSG_DATA(cmsg), &sock_fd, sizeof(sock_fd));

    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;

    if (processor->status == CHILD_STATUS_STANDBY) {
        _DBG("Activating child #%d", processor->id);
        processor->status = CHILD_STATUS_STARTING;
        child_info_image->control->new = 1;
        processor->senv->total_processors++;
        processor->senv->idle_processors++;
        processor->senv->total_processes++;
    }

    _DBG("Writing message to %d, passing sock_fd:  %d", processor->senv->output,
         sock_fd);
    _DBG("header_len=%d headers=\"%s\"", header_len, h.headers);
    _DBG("body_len=%d body=\"%s\"", body_len, body);

    if ((rv = sendmsg(processor->senv->output, &msg, 0)) == -1) {
        apr_pool_destroy(pool);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
                     "Writing message failed %d %d", rv, errno);
        return -1;
    }

    _DBG("Writing message succeeded %d", rv);

    /* -- close the socket on our side -- */
    _DBG("closing socket %d on our side", sock_fd);
    apr_socket_close(thesock);

    apr_pool_destroy(pool);
    return 1;
}

static void process_socket(apr_pool_t *p, apr_socket_t *sock, long conn_id,
        apr_bucket_alloc_t *bucket_alloc, apr_pool_t *pool)
{
    conn_rec *current_conn;
    int sock_fd;
    apr_status_t rv;
    ap_sb_handle_t *sbh;
    child_info_t *processor;
    apr_pool_t *ptrans;
    peruser_server_conf *sconf;
    int ssl_on = 0;

    _DBG("Creating dummy connection to use the vhost lookup api", 0);

    ap_create_sb_handle(&sbh, p, conn_id, 0);
    current_conn = ap_run_create_connection(p, ap_server_conf, sock, conn_id,
                                            sbh, bucket_alloc);
    _DBG("Looking up the right vhost");

    if (current_conn) {
        ap_update_vhost_given_ip(current_conn);
        _DBG("Base server is %s, name based vhosts %s",
             current_conn->base_server->server_hostname,
             current_conn->vhost_lookup_data ? "on" : "off");

        /* check for ssl configuration for this server
         * (ssl_server_is_https is NULL if we have no mod_ssl) */
        if (ssl_server_is_https) {
            ssl_on = ssl_server_is_https(current_conn->base_server);
        }
    }

    if (current_conn && (!current_conn->vhost_lookup_data || ssl_on)
            && CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_MULTIPLEXER) {
        _DBG("We are not using name based vhosts (or SSL is enabled), "
             "we'll directly pass the socket.");

        sconf = PERUSER_SERVER_CONF(current_conn->base_server->module_config);

        if (sconf->senv != NULL) {
            processor = &CHILD_INFO_TABLE[sconf->senv->processor_id];

            _DBG("Forwarding without further inspection, processor %d",
                 processor->id);

            if (processor->status == CHILD_STATUS_STANDBY) {
                _DBG("Activating child #%d", processor->id);
                processor->status = CHILD_STATUS_STARTING;
                child_info_image->control->new = 1;
                processor->senv->total_processors++;
                processor->senv->idle_processors++;
                processor->senv->total_processes++;
            }

            _DBG("Creating new pool",0);
            apr_pool_create(&ptrans, pool);

            _DBG("Passing request.",0);
            if (pass_socket(sock, processor, ptrans) == -1) {
                if (processor->senv) {
                    if (processor->senv->error_pass == 0) {
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                                     "Could not pass request to proper child, "
                                     "request will not be honoured.");
                    }
                    processor->senv->error_pass = 1;
                }
            }
            else {
                processor->senv->error_pass = 0;
            }
        }
        else {
            _DBG("Base server has no senv set!");

            if (sconf->missing_senv_reported == 0) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                             "Virtualhost %s has no server environment set, "
                             "request will not be honoured.",
                             current_conn->base_server->server_hostname);
            }

            sconf->missing_senv_reported = 1;
        }

        if (current_conn) {
            _DBG("freeing connection", 0);
            ap_lingering_close(current_conn);
        }

        _DBG("doing longjmp", 0);
        longjmp(CHILD_INFO_TABLE[my_child_num].jmpbuffer, 1);
        return;
    }

    if ((rv = apr_os_sock_get(&sock_fd, sock)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL, "apr_os_sock_get");
    }

    _DBG("child_num=%d sock=%ld sock_fd=%d", my_child_num, sock, sock_fd);
    _DBG("type=%s %d", child_type_string(CHILD_INFO_TABLE[my_child_num].type),
         my_child_num);

#ifdef _OSD_POSIX
    if (sock_fd >= FD_SETSIZE)
    {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                "new file descriptor %d is too large; you probably need "
                "to rebuild Apache with a larger FD_SETSIZE "
                "(currently %d)",
                sock_fd, FD_SETSIZE);
        apr_socket_close(sock);
        _DBG("child_num=%d: exiting with error", my_child_num);
        return;
    }
#endif

    if (CHILD_INFO_TABLE[my_child_num].sock_fd < 0) {
        ap_sock_disable_nagle(sock);
    }

    if (!current_conn) {
        ap_create_sb_handle(&sbh, p, conn_id, 0);
        current_conn = ap_run_create_connection(p, ap_server_conf, sock,
                                                conn_id, sbh, bucket_alloc);
    }

    if (current_conn) {
        ap_process_connection(current_conn, sock);
        ap_lingering_close(current_conn);
    }

}

static int peruser_process_connection(conn_rec *conn)
{
    ap_filter_t *filter;
    apr_bucket_brigade *bb;
    core_net_rec *net;

    _DBG("function entered", 0);

    /* -- fetch our sockets from the pool -- */
    apr_pool_userdata_get((void **) &bb, "PERUSER_SOCKETS", conn->pool);

    if (bb == NULL) {
        return DECLINED;
    }

    /* -- find the 'core' filter and give the socket data to it -- */
    for (filter = conn->output_filters; filter != NULL;
         filter = filter->next) {
        if (!strcmp(filter->frec->name, "core")) {
            break;
        }
    }

    if (filter == NULL) {
        return DECLINED;
    }

    net = filter->ctx;
    net->in_ctx = apr_palloc(conn->pool, sizeof(*net->in_ctx));
    net->in_ctx->b = bb;
    net->in_ctx->tmpbb = apr_brigade_create(net->in_ctx->b->p,
                                            net->in_ctx->b->bucket_alloc);

    return DECLINED;
}

static int pass_request(request_rec *r, child_info_t *child)
{
    int rv;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    apr_sockaddr_t *remote_addr;
    int sock_fd;
    char *body = "";
    struct iovec iov[5];
    conn_rec *c = r->connection;
    apr_bucket_brigade *bb = apr_brigade_create(r->pool, c->bucket_alloc);
    apr_bucket_brigade *body_bb = NULL;
    apr_size_t len = 0;
    apr_size_t header_len = 0;
    apr_size_t body_len = 0;
    peruser_header h;
    apr_bucket *bucket;
    const apr_array_header_t *headers_in_array;
    const apr_table_entry_t *headers_in;
    int counter;

    apr_socket_t *thesock = ap_get_module_config(r->connection->conn_config,
                                                 &core_module);

    if (!r->the_request || !strlen(r->the_request)) {
        _DBG("empty request. dropping it (%ld)", r->the_request);
        return -1;
    }

    if (!child) {
        _DBG("server %s in child %d has no child_info associated",
             r->hostname, my_child_num);
        return -1;
    }

    _DBG("passing request to another child.  Vhost: %s, child %d %d",
         apr_table_get(r->headers_in, "Host"), my_child_num,
         child->senv->output);

    _DBG("r->the_request=\"%s\" len=%d", r->the_request,
         strlen(r->the_request));

    /* Make sure there are free workers on the other end */
    if (child->type != CHILD_TYPE_MULTIPLEXER && wait_for_workers(child) == -1) {
        return -1;
    }

    ap_get_brigade(r->connection->input_filters, bb, AP_MODE_EXHAUSTIVE,
                   APR_NONBLOCK_READ, len);

    /* Scan the brigade looking for heap-buckets */
    _DBG("Scanning the brigade",0);
    bucket = APR_BRIGADE_FIRST(bb);

    while (bucket != APR_BRIGADE_SENTINEL(bb) && APR_BUCKET_IS_HEAP(bucket)) {
        _DBG("HEAP BUCKET is found, length=%d", bucket->length);

        bucket = APR_BUCKET_NEXT(bucket);

        if (!APR_BUCKET_IS_HEAP(bucket)) {
            _DBG("NON-HEAP BUCKET is found, extracting the part of brigade "
                 "before it");

            body_bb = bb;
            bb = apr_brigade_split(body_bb, bucket);

            /* Do we need to apr_destroy_brigade(bb) here?
             * Yeah, I know we do apr_pool_destroy(r->pool) before return, but
             * ap_get_brigade is in non-blocking mode (however len is zero).
             */
            if (apr_brigade_pflatten(body_bb, &body, &body_len, r->pool)
                != APR_SUCCESS) {

                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
                             "Unable to flatten brigade, declining request");

                apr_pool_destroy(r->pool);
                return DECLINED;
            }

            _DBG("Brigade is flattened as body (body_len=%d)", body_len);
        }
    }

    _DBG("Scanning is finished");

    apr_os_sock_get(&sock_fd, thesock);

    /* looks like a bug while sending/receiving SCM_RIGHTS related to ipv6
     workaround: send remote_addr structure too */
    apr_socket_addr_get(&remote_addr, APR_REMOTE, thesock);

    h.p = r->pool;

    headers_in_array = apr_table_elts(r->headers_in);
    headers_in = (const apr_table_entry_t *) headers_in_array->elts;

    h.headers = apr_pstrcat(h.p, r->the_request, CRLF, NULL);

    for (counter = 0; counter < headers_in_array->nelts; counter++) {
        if (headers_in[counter].key == NULL ||
            headers_in[counter].val == NULL) {
            continue;
        }

        h.headers = apr_pstrcat(h.p, h.headers, headers_in[counter].key, ": ",
                                headers_in[counter].val, CRLF, NULL);
    }

    h.headers = apr_pstrcat(h.p, h.headers, CRLF, NULL);
    ap_xlate_proto_to_ascii(h.headers, strlen(h.headers));

    header_len = strlen(h.headers);

    if (header_len > PERUSER_MAX_HEADER_SIZE) {
        _DBG("Header too big (header_len=%d)", header_len);
        return -2;
    }

    iov[0].iov_base = &header_len;
    iov[0].iov_len = sizeof(header_len);
    iov[1].iov_base = &body_len;
    iov[1].iov_len = sizeof(body_len);
    iov[2].iov_base = remote_addr;
    iov[2].iov_len = sizeof(*remote_addr);
    iov[3].iov_base = h.headers;
    iov[3].iov_len = strlen(h.headers) + 1;
    iov[4].iov_base = body;
    iov[4].iov_len = body_len;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 5;

    cmsg = apr_palloc(r->pool, sizeof(*cmsg) + sizeof(sock_fd));
    cmsg->cmsg_len = CMSG_LEN(sizeof(sock_fd));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    memcpy(CMSG_DATA(cmsg), &sock_fd, sizeof(sock_fd));

    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;

    if (child->status == CHILD_STATUS_STANDBY) {
        _DBG("Activating child #%d", child->id);
        child->status = CHILD_STATUS_STARTING;
        child_info_image->control->new = 1;
        child->senv->total_processors++;
        child->senv->idle_processors++;
        child->senv->total_processes++;
    }

    _DBG("Writing message to %d, passing sock_fd:  %d", child->senv->output,
         sock_fd);
    _DBG("header_len=%d headers=\"%s\"", header_len, h.headers);
    _DBG("body_len=%d body=\"%s\"", body_len, body);

    if ((rv = sendmsg(child->senv->output, &msg, 0)) == -1) {
        apr_pool_destroy(r->pool);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
                     "Writing message failed %d %d", rv, errno);
        return -1;
    }

    _DBG("Writing message succeeded %d", rv);

    /* -- close the socket on our side -- */
    _DBG("closing socket %d on our side", sock_fd);
    apr_socket_close(thesock);

    apr_pool_destroy(r->pool);
    return 1;
}

static apr_status_t receive_connection(void **trans_sock,
                                             ap_listen_rec *lr,
                                             apr_pool_t *ptrans)
{
    struct msghdr msg;
    struct cmsghdr *cmsg;
    char buff[PERUSER_MAX_HEADER_SIZE] = "";
    char headers[PERUSER_MAX_HEADER_SIZE] = "";
    char *body = "";
    apr_size_t header_len, body_len;
    struct iovec iov[4];
    int ret, fd_tmp;
    apr_os_sock_t ctrl_sock_fd;
    apr_os_sock_t trans_sock_fd;
    apr_sockaddr_t remote_addr;
    apr_os_sock_info_t sockinfo;

    /* -- bucket's, brigades and their allocators */
    apr_bucket_alloc_t *alloc = apr_bucket_alloc_create(ptrans);
    apr_bucket_brigade *bb = apr_brigade_create(ptrans, alloc);
    apr_bucket *bucket;

    /* prepare the buffers for receiving data from remote side */
    iov[0].iov_base = &header_len;
    iov[0].iov_len = sizeof(header_len);
    iov[1].iov_base = &body_len;
    iov[1].iov_len = sizeof(body_len);
    iov[2].iov_base = &remote_addr;
    iov[2].iov_len = sizeof(remote_addr);
    iov[3].iov_base = (char*) &buff;
    iov[3].iov_len = PERUSER_MAX_HEADER_SIZE;

    cmsg = apr_palloc(ptrans, sizeof(*cmsg) + sizeof(trans_sock_fd));
    cmsg->cmsg_len = CMSG_LEN(sizeof(trans_sock_fd));

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 4;
    msg.msg_control = cmsg;
    msg.msg_controllen = cmsg->cmsg_len;

    /* -- receive data from socket -- */
    apr_os_sock_get(&ctrl_sock_fd, lr->sd);
    _DBG("receiving from sock_fd=%d", ctrl_sock_fd);

    // Don't block
    ret = recvmsg(ctrl_sock_fd, &msg, MSG_DONTWAIT);

    if (ret == -1 && errno == EAGAIN) {
        _DBG("receive_from_multiplexer recvmsg() EAGAIN, someone was faster");

        return APR_EAGAIN;
    }
    else if (ret == -1) {
        _DBG("recvmsg failed with error \"%s\"", strerror(errno));

        // Error, better kill this child to be on the safe side
        return APR_EGENERAL;
    }
    else
        _DBG("recvmsg returned %d", ret);

    /* -- extract socket from the cmsg -- */
    memcpy(&trans_sock_fd, CMSG_DATA(cmsg), sizeof(trans_sock_fd));

    /* here *trans_sock always == NULL (socket reset at got_fd), so
     we can use apr_os_sock_make() instead of apr_os_sock_put() */

    sockinfo.os_sock = &trans_sock_fd;
    sockinfo.local = NULL;
    sockinfo.remote = (struct sockaddr *) &remote_addr.sa.sin;
    sockinfo.family = remote_addr.family;
    sockinfo.type = SOCK_STREAM;
#ifdef APR_ENABLE_FOR_1_0
    sockinfo.protocol = 0;
#endif
    apr_os_sock_make((apr_socket_t **) trans_sock, &sockinfo, ptrans);
    apr_os_sock_get(&fd_tmp, *trans_sock);

    _DBG("trans_sock=%ld fdx=%d sock_fd=%d",
            *trans_sock, trans_sock_fd, fd_tmp);

    apr_cpystrn(headers, buff, header_len + 1);
    _DBG("header_len=%d headers=\"%s\"", header_len, headers);

    if (header_len) {
        _DBG("header_len > 0, we got a request", 0);
        /* -- store received data into an brigade and add
         it to the current transaction's pool -- */
        bucket = apr_bucket_eos_create(alloc);
        APR_BRIGADE_INSERT_HEAD(bb, bucket);
        bucket = apr_bucket_socket_create(*trans_sock, alloc);
        APR_BRIGADE_INSERT_HEAD(bb, bucket);

        if (body_len) {
            body = (char*) &buff[header_len + 1];
            _DBG("body_len=%d", strlen(body));

            bucket = apr_bucket_heap_create(body, body_len, NULL, alloc);
            APR_BRIGADE_INSERT_HEAD(bb, bucket);
        }
        else {
            _DBG("There is no body",0);
        }

        bucket = apr_bucket_heap_create(headers, header_len, NULL, alloc);

        APR_BRIGADE_INSERT_HEAD(bb, bucket);
        apr_pool_userdata_set(bb, "PERUSER_SOCKETS", NULL, ptrans);
    }
    else {
        _DBG("header_len == 0, we got a socket only", 0);
    }

    return 0;
}

/* Set group privileges.
 *
 * Note that we use the username as set in the config files, rather than
 * the lookup of to uid --- the same uid may have multiple passwd entries,
 * with different sets of groups for each.
 */

static int set_group_privs(uid_t uid, gid_t gid)
{
    if (!geteuid()) {
        struct passwd *ent;
        const char *name;

        /*
         * Set the GID before initgroups(), since on some platforms
         * setgid() is known to zap the group list.
         */
        if (setgid(gid) == -1) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                         "setgid: unable to set group id to Group %u",
                         (unsigned) gid);
            return -1;
        }

        /* if getpwuid() fails, just skip initgroups() */

        if ((ent = getpwuid(uid)) != NULL) {
            name = ent->pw_name;

            /* Reset `groups' attributes. */

            if (initgroups(name, gid) == -1) {
                ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                             "initgroups: unable to set groups for User %s "
                             "and Group %u", name, (unsigned) gid);
                return -1;
            }
        }
    }
    return 0;
}

static int peruser_setup_cgroup(int childnum, server_env_t *senv,
        apr_pool_t *pool)
{
    apr_file_t *file;
    int length;
    apr_size_t content_len;
    char *tasks_file, *content, *pos;

    _DBG("starting to add pid to cgroup %s", senv->cgroup);

    length = strlen(senv->cgroup) + CGROUP_TASKS_FILE_LEN;
    tasks_file = malloc(length);

    if (!tasks_file) {
        return -1;
    }

    pos = apr_cpystrn(tasks_file, senv->cgroup, length);
    apr_cpystrn(pos, CGROUP_TASKS_FILE, CGROUP_TASKS_FILE_LEN);

    /* Prepare the data to be written to tasks file */
    content = apr_itoa(pool, ap_my_pid);
    content_len = strlen(content);

    _DBG("writing pid %s to tasks file %s", content, tasks_file);

    if (apr_file_open(&file, tasks_file, APR_WRITE, APR_OS_DEFAULT, pool)) {
        if (senv->error_cgroup == 0) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                         "cgroup: unable to open file %s", tasks_file);
        }

        senv->error_cgroup = 1;
        free(tasks_file);
        return OK; /* don't fail if cgroup not available */
    }

    if (apr_file_write(file, content, &content_len)) {
        if (senv->error_cgroup == 0) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                         "cgroup: unable to write pid to file %s", tasks_file);
        }

        senv->error_cgroup = 1;
    }
    else {
        senv->error_cgroup = 0;
    }

    apr_file_close(file);

    free(tasks_file);

    return OK;
}

static int peruser_setup_child(int childnum, apr_pool_t *pool)
{
    server_env_t *senv = CHILD_INFO_TABLE[childnum].senv;

    _DBG("function called");

    if (senv->nice_lvl != 0) {
        nice(senv->nice_lvl);
    }

    if (senv->chroot) {
        _DBG("chdir to %s", senv->chroot);

        if (chdir(senv->chroot)) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                         "chdir: unable to change to directory: %s",
                         senv->chroot);
            return -1;
        }

        if (chroot(senv->chroot)) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                         "chroot: unable to chroot to directory: %s",
                         senv->chroot);
            return -1;
        }
    }

    if (senv->cgroup) {
        peruser_setup_cgroup(childnum, senv, pool);
    }

    if (senv->uid == -1 && senv->gid == -1) {
        return unixd_setup_child();
    }

    if (set_group_privs(senv->uid, senv->gid)) {
        return -1;
    }

    /* Only try to switch if we're running as root */
    if (!geteuid() && (
#ifdef _OSD_POSIX
            os_init_job_environment(ap_server_conf, unixd_config.user_name,
                    one_process) != 0 ||
#endif
            setuid(senv->uid) == -1)) {
        ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                     "setuid: unable to change to uid: %ld", (long) senv->uid);
        return -1;
    }

    return 0;
}

static int check_signal(int signum)
{
    _DBG("signum=%d", signum);

    switch (signum) {
    case SIGTERM:
    case SIGINT:
        just_die(signum);
        return 1;
    }

    return 0;
}

/* Send a single HTTP header field to the client.  Note that this function
 * is used in calls to table_do(), so their interfaces are co-dependent.
 * In other words, don't change this one without checking table_do in alloc.c.
 * It returns true unless there was a write error of some kind.
 */
static int peruser_header_field(peruser_header *h, const char *fieldname,
                                const char *fieldval)
{
    apr_pstrcat(h->p, h->headers, fieldname, ": ", fieldval, CRLF, NULL);

    return 1;
}

static inline ap_listen_rec* listen_add(apr_pool_t* pool, apr_socket_t *sock,
        void* accept_func)
{
    ap_listen_rec *lr_walk, *lr_new;

    _DBG("function entered", 0);

    /* -- create an new listener for this child -- */
    lr_new = apr_palloc(pool, sizeof(*lr_new));
    lr_new->sd = sock;
    lr_new->active = 1;
    lr_new->accept_func = accept_func;
    lr_new->next = NULL;

    /* -- add the new listener_rec into the list -- */
    /* FIXME: should we somehow lock this list ? */
    lr_walk = ap_listeners;

    if (lr_walk) {
        while (lr_walk->next) {
            lr_walk = lr_walk->next;
        }

        lr_walk->next = lr_new;
    }
    else {
        ap_listeners = lr_walk = lr_new;
    }

    num_listensocks++;

    return lr_new;
}

static inline void listen_clear()
{
    ap_listen_rec *lr_walk;

    _DBG("function entered", 0);

    /* FIXME: should we somehow lock this list ? */
    while (ap_listeners) {
        lr_walk = ap_listeners->next;
        apr_socket_close(ap_listeners->sd);
        ap_listeners = lr_walk;
    }

    num_listensocks = 0;
}

apr_status_t cleanup_child_info(void *d)
{
    if (child_info_image == NULL) {
        return APR_SUCCESS;
    }

    free(child_info_image);
    child_info_image = NULL;
    apr_shm_destroy(child_info_shm);

    return APR_SUCCESS;
}

apr_status_t cleanup_server_environments(void *d)
{
    if (server_env_image == NULL) {
        return APR_SUCCESS;
    }

    free(server_env_image);
    server_env_image = NULL;
    apr_shm_destroy(server_env_shm);

    return APR_SUCCESS;
}

static void child_main(int child_num_arg)
{
    apr_pool_t *ptrans;
    apr_allocator_t *allocator;
    conn_rec *current_conn;
    apr_status_t status = APR_EINIT;
    int i;
    ap_listen_rec *lr;
    int curr_pollfd, last_pollfd = 0;
    apr_pollfd_t *pollset;
    int offset;
    ap_sb_handle_t *sbh;
    apr_status_t rv;
    apr_bucket_alloc_t *bucket_alloc;
    int fd;
    apr_socket_t *sock = NULL;
    apr_socket_t *pod_sock = NULL;

    /* for benefit of any hooks that run as this
     * child initializes
     */
    mpm_state = AP_MPMQ_STARTING;

    my_child_num = child_num_arg;
    ap_my_pid = getpid();
    setpgrp(ap_my_pid, ap_my_pid); 
    requests_this_child = 0;

    _DBG("sock_fd_in=%d sock_fd_out=%d",
            CHILD_INFO_TABLE[my_child_num].senv->input,
            CHILD_INFO_TABLE[my_child_num].senv->output);

    /* Get a sub context for global allocations in this child, so that
     * we can have cleanups occur when the child exits.
     */
    apr_allocator_create(&allocator);
    apr_allocator_max_free_set(allocator, ap_max_mem_free);
    apr_pool_create_ex(&pchild, pconf, NULL, allocator);
    apr_allocator_owner_set(allocator, pchild);

    apr_pool_create(&ptrans, pchild);
    apr_pool_tag(ptrans, "transaction");

    /* needs to be done before we switch UIDs so we have permissions */
    ap_reopen_scoreboard(pchild, NULL, 0);

    rv = apr_proc_mutex_child_init(&accept_mutex, ap_lock_fname, pchild);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ap_server_conf,
                     "Couldn't initialize cross-process lock in child");
        clean_child_exit(APEXIT_CHILDFATAL);
    }

    switch (CHILD_INFO_TABLE[my_child_num].type) {
    case CHILD_TYPE_MULTIPLEXER:
        _DBG("MULTIPLEXER %d", my_child_num);

        /* -- create new listener to receive from workers -- */
        apr_os_sock_put(&sock, &CHILD_INFO_TABLE[my_child_num].senv->input,
                        pconf);
        listen_add(pconf, sock, receive_connection);
        break;

    case CHILD_TYPE_PROCESSOR:
    case CHILD_TYPE_WORKER:
        _DBG("%s %d", child_type_string(CHILD_INFO_TABLE[my_child_num].type),
             my_child_num);

        /* -- create new listener to receive from multiplexer -- */
        apr_os_sock_put(&sock, &CHILD_INFO_TABLE[my_child_num].senv->input,
                        pconf);
        listen_clear();
        listen_add(pconf, sock, receive_connection);

        break;

    default:
        _DBG("unspecified child type for %d sleeping a while ...",
             my_child_num);

        apr_sleep(apr_time_from_sec(5));
        return;
    }

    apr_os_file_get(&fd, pipe_of_death_in);
    apr_os_sock_put(&pod_sock, &fd, pconf);
    listen_add(pconf, pod_sock, check_pipe_of_death);

    if (peruser_setup_child(my_child_num, pchild) != 0) {
        clean_child_exit(APEXIT_CHILDFATAL);
    }

    ap_run_child_init(pchild, ap_server_conf);

    ap_create_sb_handle(&sbh, pchild, my_child_num, 0);
    (void) ap_update_child_status(sbh, SERVER_READY, (request_rec *) NULL);

    /* Set up the pollfd array */
    listensocks = apr_pcalloc(pchild, sizeof(*listensocks) * (num_listensocks));

    for (lr = ap_listeners, i = 0; i < num_listensocks; lr = lr->next, i++) {
        listensocks[i].accept_func = lr->accept_func;
        listensocks[i].sd = lr->sd;
    }

    pollset = apr_palloc(pchild, sizeof(*pollset) * num_listensocks);
    pollset[0].p = pchild;
    for (i = 0; i < num_listensocks; i++) {
        pollset[i].desc.s = listensocks[i].sd;
        pollset[i].desc_type = APR_POLL_SOCKET;
        pollset[i].reqevents = APR_POLLIN;
    }

    mpm_state = AP_MPMQ_RUNNING;

    bucket_alloc = apr_bucket_alloc_create(pchild);

    while (!die_now) {
        /*
         * (Re)initialize this child to a pre-connection state.
         */

        current_conn = NULL;

        apr_pool_clear(ptrans);

        if (CHILD_INFO_TABLE[my_child_num].type != CHILD_TYPE_MULTIPLEXER
            && ap_max_requests_per_child > 0
            && requests_this_child++ >= ap_max_requests_per_child) {
            _DBG("max requests reached, dying now", 0);
            clean_child_exit(0);
        }

        (void) ap_update_child_status(sbh, SERVER_READY, (request_rec *) NULL);

        CHILD_INFO_TABLE[my_child_num].status = CHILD_STATUS_READY;
        CHILD_INFO_TABLE[my_child_num].cpu_stopped = 0;
        CHILD_INFO_TABLE[my_child_num].cpu_usage = 0;
        _DBG("Child %d (%s) is now ready", my_child_num,
             child_type_string(CHILD_INFO_TABLE[my_child_num].type));

        /*
         * Wait for an acceptable connection to arrive.
         */

        /* Lock around "accept", if necessary */
        if (CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_MULTIPLEXER) {
            SAFE_ACCEPT(accept_mutex_on());
        }

        if (num_listensocks == 1) {
            offset = 0;
        }
        else {
            /* multiple listening sockets - need to poll */
            for (;;) {
                apr_status_t ret;
                apr_int32_t n;

                ret = apr_poll(pollset, num_listensocks, &n, -1);

                if (ret != APR_SUCCESS) {
                    if (APR_STATUS_IS_EINTR(ret)) {
                        continue;
                    }

                    /* Single Unix documents select as returning errnos
                     * EBADF, EINTR, and EINVAL... and in none of those
                     * cases does it make sense to continue.  In fact
                     * on Linux 2.0.x we seem to end up with EFAULT
                     * occasionally, and we'd loop forever due to it.
                     */
                    ap_log_error(APLOG_MARK, APLOG_ERR, ret, ap_server_conf,
                                 "apr_poll: (listen)");
                    clean_child_exit(1);
                }

                /* find a listener */
                curr_pollfd = last_pollfd;
                do {
                    curr_pollfd++;

                    if (curr_pollfd >= num_listensocks) {
                        curr_pollfd = 0;
                    }

                    /* XXX: Should we check for POLLERR? */
                    if (pollset[curr_pollfd].rtnevents & APR_POLLIN) {
                        last_pollfd = curr_pollfd;
                        offset = curr_pollfd;
                        goto got_fd;
                    }
                }
                while (curr_pollfd != last_pollfd);

                continue;
            }
        }

        got_fd:

        _DBG("input available ... resetting socket.",0);
        sock = NULL; /* important! */

        /* if we accept() something we don't want to die, so we have to
         * defer the exit
         */
        status = listensocks[offset].accept_func((void *) &sock,
                                                 &listensocks[offset], ptrans);

        if (CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_MULTIPLEXER) {
            SAFE_ACCEPT(accept_mutex_off()); /* unlock after "accept" */
        }

        if (status == APR_EGENERAL) {
            /* resource shortage or should-not-occur occured */
            clean_child_exit(1);
        }
        else if (status != APR_SUCCESS || die_now || sock == NULL) {
            continue;
        }

        if (CHILD_INFO_TABLE[my_child_num].status == CHILD_STATUS_READY) {
            CHILD_INFO_TABLE[my_child_num].status = CHILD_STATUS_ACTIVE;
            CHILD_INFO_TABLE[my_child_num].senv->idle_processors--;
            _DBG("Child %d (%s) is now active", my_child_num,
                 child_type_string(CHILD_INFO_TABLE[my_child_num].type));
        }

        if (CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_PROCESSOR
            || CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_WORKER
            || CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_MULTIPLEXER) {

            _DBG("CHECKING IF WE SHOULD CLONE A CHILD...");

            update_single_processor_counters(my_child_num);

            _DBG("total_processors = %d, max_processors = %d",
                    CHILD_INFO_TABLE[my_child_num].senv->total_processors,
                    CHILD_INFO_TABLE[my_child_num].senv->max_processors);

            _DBG("idle_processors = %d, min_free_processors = %d",
                    CHILD_INFO_TABLE[my_child_num].senv->idle_processors,
                    CHILD_INFO_TABLE[my_child_num].senv->min_free_processors);

            if (CHILD_INFO_TABLE[my_child_num].senv->total_processors
                < CHILD_INFO_TABLE[my_child_num].senv->max_processors
                && (CHILD_INFO_TABLE[my_child_num].senv->idle_processors
                <= CHILD_INFO_TABLE[my_child_num].senv->min_free_processors
                || CHILD_INFO_TABLE[my_child_num].senv->total_processors
                < CHILD_INFO_TABLE[my_child_num].senv->min_processors)) {

                _DBG("CLONING CHILD");
                child_clone(-1); // -1 => this child
            }
        }

        if (!setjmp(CHILD_INFO_TABLE[my_child_num].jmpbuffer)) {
            _DBG("marked jmpbuffer",0);

            CHILD_INFO_TABLE[my_child_num].senv->stats_connections++;

            _TRACE_CALL("process_socket()",0);

            process_socket(ptrans, sock, my_child_num, bucket_alloc, pchild);

            _TRACE_RET("process_socket()",0);
        }
        else {
            _DBG("landed from longjmp",0);
            CHILD_INFO_TABLE[my_child_num].sock_fd = AP_PERUSER_THISCHILD;
        }

        /* Check the pod and the generation number after processing a
         * connection so that we'll go away if a graceful restart occurred
         * while we were processing the connection or we are the lucky
         * idle server process that gets to die.
         */
        if (ap_mpm_pod_check(pod) == APR_SUCCESS) { /* selected as idle? */
            _DBG("ap_mpm_pod_check(pod) = APR_SUCCESS; dying now", 0);
            die_now = 1;
        }
        else if (ap_my_generation
                 != ap_scoreboard_image->global->running_generation) {
            /* yeah, this could be non-graceful restart, in which case the
             * parent will kill us soon enough, but why bother checking?
             */
            _DBG("ap_my_generation !="
                 " ap_scoreboard_image->global->running_generation; dying now");
            die_now = 1;
        }

        if (CHILD_INFO_TABLE[my_child_num].status == CHILD_STATUS_RESTART) {
            _DBG("restarting", 0);
            die_now = 1;
        }
    }

    CHILD_INFO_TABLE[my_child_num].cpu_stopped = 0;
    CHILD_INFO_TABLE[my_child_num].cpu_usage = 0;

    // Call apr_pool_clear() - Apache prefork bug (PR 43857)
    apr_pool_clear(ptrans);
    _DBG("clean_child_exit(0)");
    clean_child_exit(0);
}

static server_env_t* find_senv_by_name(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }

    _DBG("name=%s", name);

    for (i = 0; i < NUM_SENV; i++) {
        if (SENV[i].name != NULL && !strcmp(SENV[i].name, name)) {
            return &SENV[i];
        }
    }

    return NULL;
}

static server_env_t* find_matching_senv(server_env_t* senv)
{
    int i;

    _DBG("name=%s uid=%d gid=%d chroot=%s", senv->name, senv->uid, senv->gid,
         senv->chroot);

    for (i = 0; i < NUM_SENV; i++) {
        if ((senv->name != NULL && SENV[i].name != NULL
             && !strcmp(SENV[i].name, senv->name))
            || (senv->name == NULL && SENV[i].uid == senv->uid
                && SENV[i].gid == senv->gid
                && ((SENV[i].chroot == NULL && senv->chroot == NULL)
                    || ((SENV[i].chroot != NULL || senv->chroot != NULL)
                    && !strcmp(SENV[i].chroot, senv->chroot))))) {
            return &SENV[i];
        }
    }

    return NULL;
}

static server_env_t* senv_add(server_env_t *senv)
{
    int socks[2];
    server_env_t *old_senv;

    _DBG("Searching for matching senv...");

    old_senv = find_matching_senv(senv);

    if (old_senv) {
        _DBG("Found existing senv");

        senv = old_senv;

        return old_senv;
    }

    if (NUM_SENV >= server_limit) {
        _DBG("server_limit reached!");
        return NULL;
    }

    _DBG("Creating new senv");

    memcpy(&SENV[NUM_SENV], senv, sizeof(server_env_t));

    SENV[NUM_SENV].availability = 100;

    socketpair(PF_UNIX, SOCK_STREAM, 0, socks);
    SENV[NUM_SENV].input = socks[0];
    SENV[NUM_SENV].output = socks[1];

    _DBG("New senv id %d", NUM_SENV);
    SENV[NUM_SENV].id = NUM_SENV;

    senv = &SENV[NUM_SENV];
    return &SENV[server_env_image->control->num++];
}

static void child_clone(int child_num)
{
    int i, child_to_clone;
    child_info_t *this;
    child_info_t *new;

    if (child_num == -1)
        child_to_clone = my_child_num;
    else
        child_to_clone = child_num;

    for (i = 0; i < NUM_CHILDS; i++) {
        if (CHILD_INFO_TABLE[i].pid == 0
            && CHILD_INFO_TABLE[i].type == CHILD_TYPE_UNKNOWN) {
            CHILD_INFO_TABLE[i].type = CHILD_TYPE_RESERVED;
            break;
        }
    }

    if (i == NUM_CHILDS && NUM_CHILDS >= server_limit) {
        _DBG("Trying to use more child ID's than ServerLimit.  "
                "Increase ServerLimit in your config file.");
        return;
    }

    _DBG("cloning child #%d from #%d", i, child_to_clone);

    this = &CHILD_INFO_TABLE[child_to_clone];
    new = &CHILD_INFO_TABLE[i];

    new->senv = this->senv;

    if (this->type == CHILD_TYPE_MULTIPLEXER) {
        new->type = CHILD_TYPE_MULTIPLEXER;
    }
    else {
        new->type = CHILD_TYPE_WORKER;
    }

    new->sock_fd = this->sock_fd;
    new->status = CHILD_STATUS_STARTING;
    if (child_num == -1) child_info_image->control->new = 1;
    new->senv->total_processors++;
    new->senv->idle_processors++;
    new->senv->total_processes++;

    if (i == NUM_CHILDS) {
        child_info_image->control->num++;
    }

    return;
}

static const char* child_add(int type, int status, apr_pool_t *pool,
                             server_env_t *senv)
{
    int i;

    if (senv->chroot && !ap_is_directory(pool, senv->chroot)) {
        return apr_psprintf(pool,
                            "Error: chroot directory [%s] does not exist",
                            senv->chroot);
    }

    for (i = 0; i < NUM_CHILDS; i++) {
        if (CHILD_INFO_TABLE[i].pid == 0
            && CHILD_INFO_TABLE[i].type == CHILD_TYPE_UNKNOWN) {
            CHILD_INFO_TABLE[i].type = CHILD_TYPE_RESERVED;
            break;
        }
    }

    _DBG("adding child #%d", i);

    if (i >= server_limit) {
        return "Trying to use more child ID's than ServerLimit.  "
            "Increase ServerLimit in your config file.";
    }

    CHILD_INFO_TABLE[i].senv = senv_add(senv);

    if (CHILD_INFO_TABLE[i].senv == NULL) {
        // Changing back to UNKNOWN type
        CHILD_INFO_TABLE[i].type = CHILD_TYPE_UNKNOWN;
        return "Trying to use more server environments than ServerLimit.  "
            "Increase ServerLimit in your config file.";
    }

    if (type == CHILD_TYPE_MULTIPLEXER) {
        multiplexer_senv = CHILD_INFO_TABLE[i].senv;
    }

    if (type != CHILD_TYPE_WORKER) {
        CHILD_INFO_TABLE[i].senv->processor_id = i;
    }

    CHILD_INFO_TABLE[i].type = type;
    CHILD_INFO_TABLE[i].sock_fd = AP_PERUSER_THISCHILD;
    CHILD_INFO_TABLE[i].status = status;

    _DBG("[%d] uid=%d gid=%d type=%d chroot=%s", i, senv->uid,
         senv->gid, type, senv->chroot);

    if (senv->uid == 0 || senv->gid == 0) {
        _DBG("Assigning root user/group to a child.", 0);
    }

    if (i >= NUM_CHILDS) {
        child_info_image->control->num = i + 1;
    }

    return NULL;
}

static int make_child(server_rec *s, int slot)
{
    int pid;

    _DBG("function entered", 0);
    dump_server_env_image();

    switch (CHILD_INFO_TABLE[slot].type) {
    case CHILD_TYPE_MULTIPLEXER:
        break;
    case CHILD_TYPE_PROCESSOR:
        break;
    case CHILD_TYPE_WORKER:
        break;

    default:
        _DBG("no valid client in slot %d", slot);
        /* sleep(1); */
        return 0;
    }

    if (one_process) {
        apr_signal(SIGHUP, just_die);
        /* Don't catch AP_SIG_GRACEFUL in ONE_PROCESS mode :) */
        apr_signal(SIGINT, just_die);
#ifdef SIGQUIT
        apr_signal(SIGQUIT, SIG_DFL);
#endif
        apr_signal(SIGTERM, just_die);
        child_main(slot);
    }

    (void) ap_update_child_status_from_indexes(slot, 0, SERVER_STARTING,
                                               (request_rec *) NULL);

    CHILD_INFO_TABLE[slot].status = CHILD_STATUS_READY;

#ifdef _OSD_POSIX
    /* BS2000 requires a "special" version of fork() before a setuid() call */
    if ((pid = os_fork(unixd_config.user_name)) == -1) {
#elif defined(TPF)
        if ((pid = os_fork(s, slot)) == -1) {
#else
    if ((pid = fork()) == -1) {
#endif
        ap_log_error(APLOG_MARK, APLOG_ERR, errno, s,
                     "fork: Unable to fork new process");

        /* fork didn't succeed. Fix the scoreboard or else
         * it will say SERVER_STARTING forever and ever
         */
        (void) ap_update_child_status_from_indexes(slot, 0, SERVER_DEAD,
                                                   (request_rec *) NULL);

        /* In case system resources are maxxed out, we don't want
         Apache running away with the CPU trying to fork over and
         over and over again. */
        apr_sleep(apr_time_from_sec(10));

        return -1;
    }

    if (!pid) {
#ifdef HAVE_BINDPROCESSOR
        /* by default AIX binds to a single processor
         * this bit unbinds children which will then bind to another cpu
         */
        int status = bindprocessor(BINDPROCESS, (int)getpid(),
                PROCESSOR_CLASS_ANY);
        if (status != OK) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno,
                    ap_server_conf, "processor unbind failed %d", status);
        }
#endif
        RAISE_SIGSTOP(MAKE_CHILD); AP_MONCONTROL(1);
        /* Disable the parent's signal handlers and set up proper handling in
         * the child.
         */
        apr_signal(SIGHUP, just_die);
        apr_signal(SIGTERM, just_die);
        /* The child process doesn't do anything for AP_SIG_GRACEFUL.  
         * Instead, the pod is used for signalling graceful restart.
         */
        /* apr_signal(AP_SIG_GRACEFUL, restart); */
        child_main(slot);
        clean_child_exit(0);
    }

    ap_scoreboard_image->parent[slot].pid = pid;
    CHILD_INFO_TABLE[slot].pid = pid;
    CHILD_INFO_TABLE[slot].senv->active_processors++;

    return 0;
}

/*
 * idle_spawn_rate is the number of children that will be spawned on the
 * next maintenance cycle if there aren't enough idle servers.  It is
 * doubled up to MAX_SPAWN_RATE, and reset only when a cycle goes by
 * without the need to spawn.
 */
static int idle_spawn_rate = 1;
#ifndef MAX_SPAWN_RATE
#define MAX_SPAWN_RATE	(32)
#endif

static inline int determine_child_fate(int childnum, child_info_t *child,
                                worker_score *child_sb, apr_time_t now)
{
    time_t idle_time = apr_time_sec(now - child_sb->last_used);

    /* All active processors must be killed after ExpireTimeout, regardless
       MinProcessors, or infinite loop processors will never end  */       
    if ((child->type == CHILD_TYPE_PROCESSOR ||
         child->type == CHILD_TYPE_WORKER)
        && child->status == CHILD_STATUS_ACTIVE
        && expire_timeout > 0 && idle_time > expire_timeout) {
        /* Child has not handled a request for some time now, stop it */
        _DBG("First expire timeout reached for child #%d", childnum);
        return 1;
    }

    if (child->senv->min_processors > 0 &&
        child->senv->total_processes <= child->senv->min_processors) {
        /* We will not kill a child, if the senv needs live workers */
        return 0;
    }

    if (child->type == CHILD_TYPE_PROCESSOR ||
        child->type == CHILD_TYPE_WORKER) {

        /*
        If MinProcessors AND MinSpareProcessors are set to a positive
        value, server will keep idle processors, regardless IdleTimeout
        configuration, but if MinProcessors is 0, server will keep idle
        processors UNTIL IdleTimeout.
        */
        if (child->senv->min_processors > 0 &&
            child->senv->min_free_processors > 0 &&
            child->senv->idle_processors <= child->senv->min_free_processors) {
            /* We will not kill a child, if the senv needs idle workers */
            return 0;
        }

        /* TODO: Maybe this step will be removed from here */
        if (expire_timeout > 0 && idle_time > expire_timeout) {
            /* Child has not handled a request for some time now, stop it */
            _DBG("Second expire timeout reached for child #%d", childnum);
            return 1;
        }

        /* Never kill the last idle child if all others are busy */
        if (child->senv->total_processes > 0
            && child->senv->active_processors >= child->senv->total_processors
            && child->senv->idle_processors == 1
            && child_sb->status == SERVER_READY) {
            /* We will not kill this child, this is the last idle child,
               all others are busy */
            _DBG("Child %d not killed, all others are busy", childnum);
            return 0;
        }

        if (idle_timeout > 0 && child_sb->status == SERVER_READY
            && idle_time > idle_timeout) {
            /* Child has been idle for too long, stop it */
            _DBG("Idle timeout reached for child #%d", childnum);
            return 1;
        }

        /* Dont kill a recent created child */
        if (idle_time > 0 && child->type == CHILD_TYPE_WORKER
            /* && child->status == CHILD_STATUS_READY */
            && child->senv->max_free_processors > 0
            && child->senv->idle_processors >= child->senv->max_free_processors) {
            /* Too many spare workers available */
            _DBG("Too many spare workers for processor %s, stopping child #%d",
                 child->senv->name, childnum);
            return 1;
        }
    } else if (child->type == CHILD_TYPE_MULTIPLEXER) {
        if (multiplexer_idle_timeout > 0 && child_sb->status == SERVER_READY
            && idle_time > multiplexer_idle_timeout) {
            /* Multiplexer has been idle for too long, stop it */
            _DBG("Stopping idle multiplexer #%d", childnum);
            return 1;
        }
    }

    return 0;
}

static void inline create_new_childs()
{
    int i;

    for (i = 0; i < NUM_CHILDS; ++i) {
        if (restart_pending) {
            _DBG("Exiting... restart_pending = %d", restart_pending);
            break;
        }
        if (CHILD_INFO_TABLE[i].status == CHILD_STATUS_STARTING)
            make_child(ap_server_conf, i);
    }
}

static void inline check_for_new_childs()
{
    int i;

    for (i = 0; i < 50; ++i) {
        if (child_info_image->control->new) {
            create_new_childs();
            child_info_image->control->new = 0;
        }
        apr_sleep(SCOREBOARD_MAINTENANCE_INTERVAL / 50);
    }
}

static void perform_idle_server_maintenance(apr_pool_t *p)
{
    int i, stop_child, retval;
    time_t idle_time;
    child_info_t *child;
    worker_score *child_sb;
    apr_time_t now;
    now = apr_time_now();
    time_t up_time = apr_time_sec(now - ap_scoreboard_image->global->restart_time);

    _DBG("function entered");

    cpu_usage_refresh();
    update_all_counters();

    for (i = 0; i < NUM_CHILDS; ++i) {
        if (restart_pending) {
            _DBG("Exiting... restart_pending = %d", restart_pending);
            break;
        }

        child = &CHILD_INFO_TABLE[i];
        child_sb = &ap_scoreboard_image->servers[i][0];

        if (child->status == CHILD_STATUS_STANDBY) {
            continue;
        }

        if (child->pid == 0) {

            /* If some child unexpectedly dies, we should free this child */
            if (child->type > CHILD_TYPE_UNKNOWN &&
                (child->status == CHILD_STATUS_ACTIVE || child->status == CHILD_STATUS_READY)
                && child_sb->status == SERVER_DEAD) {
                _DBG("Something is wrong with child #%d", child->id);
                if (child->type != CHILD_TYPE_MULTIPLEXER && child->senv) {
                     retval = close(child->senv->input);
                     _DBG("close(CHILD_INFO_TABLE[%d].senv->input) = %d",
                          i, retval);
                     retval = close(child->senv->output);
                     _DBG("close(CHILD_INFO_TABLE[%d].senv->output) = %d",
                          i, retval);
                }
                if (child->type != CHILD_TYPE_PROCESSOR) {
                    /* completely free up this slot */
                    child->senv = (server_env_t*) NULL;
                    child->type = CHILD_TYPE_UNKNOWN;
                    child->sock_fd = -3; /* -1 and -2 are taken */
                    child->cpu_stopped = 0;
                }
                child->status = CHILD_STATUS_STANDBY;
            }

            if (child->status == CHILD_STATUS_STARTING) {
                make_child(ap_server_conf, i);
            }

            continue;
        }

        if (ap_scoreboard_image->parent[i].pid == 0) {
            continue;
        }

        /* Wait 20 seconds to start CPU control */
        if (up_time >= 20 && child->senv->max_cpu_usage > 0) {
            child->cpu_usage = cpu_usage_pid(ap_scoreboard_image->parent[i].pid);
            _DBG("DEBUG pid = %d, cpu_usage = %.1f, cpu_stopped = %d", ap_scoreboard_image->parent[i].pid, child->cpu_usage, child->cpu_stopped);
            if (child->type != CHILD_TYPE_MULTIPLEXER && child->status == CHILD_STATUS_ACTIVE) {
                if (child->cpu_stopped) {
                    if (child->cpu_usage < child->senv->max_cpu_usage) {
                        child->cpu_stopped--;
                        if (child->cpu_stopped <= 0) {
                            unixd_killpg(ap_scoreboard_image->parent[i].pid, SIGCONT);
                            child->cpu_stopped = 0;
                        }
                    } else {
                        if (child->cpu_stopped < 2) child->cpu_stopped++;
                    }
                } else {
                    if (child->cpu_usage >= child->senv->max_cpu_usage) {
                        unixd_killpg(ap_scoreboard_image->parent[i].pid, SIGSTOP);
                        child->cpu_stopped = 1;
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                                     "Processor %s stopped (pid = %d, cpu_usage = %.1f, max_cpu_usage = %d)",
                                     child->senv->name, ap_scoreboard_image->parent[i].pid,
                                     child->cpu_usage, child->senv->max_cpu_usage);
                    }
                }
            }
        } else {
            if (child->cpu_stopped) {
                unixd_killpg(ap_scoreboard_image->parent[i].pid, SIGCONT);
                child->cpu_stopped = 0;
            }
        }

        if (determine_child_fate(i, child, child_sb, now) == 1) {

            /* updating counters */
            if (child->status == CHILD_STATUS_READY)
                child->senv->idle_processors--;
            if (child->pid > 0)
                child->senv->active_processors--;
            if (child->status != CHILD_STATUS_STANDBY)
                child->senv->total_processes--;
            child->senv->total_processors--;

            child->status = CHILD_STATUS_STANDBY;

            if (unixd_killpg(ap_scoreboard_image->parent[i].pid, SIGTERM) == -1) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                             "kill SIGTERM");
            }

            if (child->cpu_stopped) {
                unixd_killpg(ap_scoreboard_image->parent[i].pid, SIGCONT);
                child->cpu_stopped = 0;
            }
            child->cpu_usage = 0;

            /* If all processors are busy, we will clone a child */
            if (child->senv->idle_processors == 0
                && child->senv->total_processors > 0
                && child->senv->total_processors < child->senv->max_processors
                && child->senv->total_processors <= child->senv->total_processes) {
                _DBG("CLONING CHILD %d", child->id);
                child_clone(child->id);
            }

        }
    }

}

/*****************************************************************
 * Executive routines.
 */

int ap_mpm_run(apr_pool_t *_pconf, apr_pool_t *plog, server_rec *s)
{
    int i;
    /*    int fd; */
    apr_status_t rv;
    apr_size_t one = 1;
    unsigned char status;
    /*    apr_socket_t *sock = NULL; */

    ap_log_pid(pconf, ap_pid_fname);

    first_server_limit = server_limit;
    if (changed_limit_at_restart) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s,
                     "WARNING: Attempt to change ServerLimit "
                     "ignored during restart");
        changed_limit_at_restart = 0;
    }

    ap_server_conf = s;

    /* Initialize cross-process accept lock */
    ap_lock_fname = apr_psprintf(_pconf, "%s.%" APR_PID_T_FMT,
                                 ap_server_root_relative(_pconf, ap_lock_fname),
                                 ap_my_pid);

    rv = apr_proc_mutex_create(&accept_mutex, ap_lock_fname,
                               ap_accept_lock_mech, _pconf);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
                     "Couldn't create accept lock");
        mpm_state = AP_MPMQ_STOPPING;
        return 1;
    }

#if APR_USE_SYSVSEM_SERIALIZE
    if (ap_accept_lock_mech == APR_LOCK_DEFAULT ||
            ap_accept_lock_mech == APR_LOCK_SYSVSEM) {
#else
    if (ap_accept_lock_mech == APR_LOCK_SYSVSEM) {
#endif
        rv = unixd_set_proc_mutex_perms(accept_mutex);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
                         "Couldn't set permissions on cross-process lock; "
                             "check User and Group directives");
            mpm_state = AP_MPMQ_STOPPING;
            return 1;
        }
    }

    if (!is_graceful) {
        if (ap_run_pre_mpm(s->process->pool, SB_SHARED) != OK) {
            mpm_state = AP_MPMQ_STOPPING;
            return 1;
        }
        /* fix the generation number in the global score; we just got a new,
         * cleared scoreboard
         */
        ap_scoreboard_image->global->running_generation = ap_my_generation;
    }

    /* We need to put the new listeners at the end of the ap_listeners
     * list.  If we don't, then the pool will be cleared before the
     * open_logs phase is called for the second time, and ap_listeners
     * will have only invalid data.  If that happens, then the sockets
     * that we opened using make_sock() will be lost, and the server
     * won't start.
     */

    /*
     apr_os_file_get(&fd, pipe_of_death_in);
     apr_os_sock_put(&sock, &fd, pconf);

     listen_add(pconf, sock, check_pipe_of_death);
     */
    set_signals();

    if (one_process) {
        AP_MONCONTROL(1);
    }

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                 "%s configured -- resuming normal operations",
                 ap_get_server_version());
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, ap_server_conf, "Server built: %s",
                 ap_get_server_built());
#ifdef AP_MPM_WANT_SET_ACCEPT_LOCK_MECH
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
            "AcceptMutex: %s (default: %s)",
            apr_proc_mutex_name(accept_mutex),
            apr_proc_mutex_defname());
#endif
    restart_pending = shutdown_pending = 0;

    mpm_state = AP_MPMQ_RUNNING;

    _DBG("sizeof(child_info_t) = %d", sizeof(child_info_t));

    while (!restart_pending && !shutdown_pending) {
        int child_slot = -1;
        apr_exit_why_e exitwhy;
        int status, processed_status;
        /* this is a memory leak, but I'll fix it later. */
        apr_proc_t pid;

        ap_wait_or_timeout(&exitwhy, &status, &pid, pconf);

        /* XXX: if it takes longer than 1 second for all our children
         * to start up and get into IDLE state then we may spawn an
         * extra child
         */
        if (pid.pid != -1) {
            processed_status = ap_process_child_status(&pid, exitwhy, status);
            if (processed_status == APEXIT_CHILDFATAL) {
                mpm_state = AP_MPMQ_STOPPING;
                return 1;
            }

            /* non-fatal death... note that it's gone in the scoreboard. */
            for (i = 0; i < NUM_CHILDS; ++i) {
                if (CHILD_INFO_TABLE[i].pid == pid.pid) {
                    child_slot = i;
                    break;
                }
            }

            _DBG("child #%d (pid=%d) has died", child_slot, pid.pid);

            if (child_slot >= 0) {
                CHILD_INFO_TABLE[child_slot].pid = 0;

                status = ap_scoreboard_image->servers[child_slot][0].status;

                _TRACE_CALL("ap_update_child_status_from_indexes", 0);

                (void)ap_update_child_status_from_indexes(child_slot, 0,
                                                          SERVER_DEAD,
                                                          (request_rec *)NULL);

                _TRACE_RET("ap_update_child_status_from_indexes", 0);

                if (processed_status == APEXIT_CHILDSICK) {
                    /* child detected a resource shortage (E[NM]FILE, ENOBUFS,
                     * etc)
                     * cut the fork rate to the minimum 
                     */
                    _DBG("processed_status = APEXIT_CHILDSICK", 0);
                    idle_spawn_rate = 1;
                }
                else if (status == SERVER_GRACEFUL) {
                    _DBG("cleaning child from last generation");
                    memset(&CHILD_INFO_TABLE[child_slot], 0, sizeof(child_info_t));
                    CHILD_INFO_TABLE[child_slot].id = child_slot;
                    CHILD_INFO_TABLE[child_slot].cpu_stopped = 0;
                    CHILD_INFO_TABLE[child_slot].cpu_usage = 0;
                }
                else if (CHILD_INFO_TABLE[child_slot].status
                        == CHILD_STATUS_STANDBY)
                {
                    _DBG("leaving child in standby state", 0);

                    if (CHILD_INFO_TABLE[child_slot].type == CHILD_TYPE_WORKER
                        || CHILD_INFO_TABLE[child_slot].type == CHILD_TYPE_MULTIPLEXER) {
                        /* completely free up this slot */
                        CHILD_INFO_TABLE[child_slot].senv = (server_env_t*) NULL;
                        CHILD_INFO_TABLE[child_slot].type = CHILD_TYPE_UNKNOWN;
                        CHILD_INFO_TABLE[child_slot].sock_fd = -3; /* -1 and -2 are taken */
                    }
                }
                else if (child_slot < ap_daemons_limit
                        && CHILD_INFO_TABLE[child_slot].type
                                > CHILD_TYPE_UNKNOWN)
                {
                    /* we're still doing a 1-for-1 replacement of dead
                     * children with new children
                     */
                    _DBG("replacing by new child ...", 0);
                    make_child(ap_server_conf, child_slot);
                }
#if APR_HAS_OTHER_CHILD
            }
            else if (apr_proc_other_child_alert(&pid, APR_OC_REASON_DEATH,
                                                status) == APR_SUCCESS) {
                _DBG("Already handled", 0);
                /* handled */
#endif
            }
            else if (is_graceful) {
                /* Great, we've probably just lost a slot in the
                 * scoreboard.  Somehow we don't know about this
                 * child.
                 */
                _DBG("long lost child came home, whatever that means", 0);

                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ap_server_conf,
                             "long lost child came home! (pid %ld)",
                             (long) pid.pid);
            }
            /* Don't perform idle maintenance when a child dies,
             * only do it when there's a timeout.  Remember only a
             * finite number of children can die, and it's pretty
             * pathological for a lot to die suddenly.
             */
            continue;
        }

        /* Create new processors while waiting */
        check_for_new_childs();
        perform_idle_server_maintenance(pconf);

#ifdef TPF
        shutdown_pending = os_check_server(tpf_server_name);
        ap_check_signals();
        apr_sleep(apr_time_from_sec(1));
#endif /*TPF */
    }

    cpu_usage_finish();

    mpm_state = AP_MPMQ_STOPPING;

    if (shutdown_pending) {
        /* Time to gracefully shut down:
         * Kill child processes, tell them to call child_exit, etc...
         */

        int n = 0;
        for (n = 0; n < NUM_CHILDS; ++n) {
            if (ap_scoreboard_image->servers[n][0].status != SERVER_DEAD &&
                ap_scoreboard_image->parent[n].pid) {
                    unixd_killpg(ap_scoreboard_image->parent[n].pid, SIGTERM);
                    if (CHILD_INFO_TABLE[n].cpu_stopped) {
                        unixd_killpg(ap_scoreboard_image->parent[n].pid, SIGCONT);
                        CHILD_INFO_TABLE[n].cpu_stopped = 0;
                    }
                    CHILD_INFO_TABLE[n].cpu_usage = 0;
            }
        }
 
        if (unixd_killpg(getpgrp(), SIGTERM) < 0) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "killpg SIGTERM");
        }
        ap_reclaim_child_processes(1); /* Start with SIGTERM */

        /* cleanup pid file on normal shutdown */
        {
            const char *pidfile = NULL;
            pidfile = ap_server_root_relative(pconf, ap_pid_fname);
            if (pidfile != NULL && unlink(pidfile) == 0)
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, ap_server_conf,
                             "removed PID file %s (pid=%ld)", pidfile,
                             (long) getpid());
        }

        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                     "caught SIGTERM, shutting down");
        return 1;
    }

    /* we've been told to restart */
    apr_signal(SIGHUP, SIG_IGN);
    if (one_process) {
        /* not worth thinking about */
        return 1;
    }

    /* advance to the next generation */
    /* XXX: we really need to make sure this new generation number isn't in
     * use by any of the children.
     */
    ++ap_my_generation;
    ap_scoreboard_image->global->running_generation = ap_my_generation;

    /* cleanup sockets */
    for (i = 0; i < NUM_SENV; i++) {
        close(SENV[i].input);
        close(SENV[i].output);
    }

    if (is_graceful) {
        char char_of_death = AP_PERUSER_CHAR_OF_DEATH;

        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                     "Graceful restart requested, doing restart");

        /* This is mostly for debugging... so that we know what is still
         * gracefully dealing with existing request.  This will break
         * in a very nasty way if we ever have the scoreboard totally
         * file-based (no shared memory)
         */
        for (i = 0; i < ap_daemons_limit; ++i) {
            if (ap_scoreboard_image->servers[i][0].status != SERVER_DEAD) {
                ap_scoreboard_image->servers[i][0].status = SERVER_GRACEFUL;
            }
        }

        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                     AP_SIG_GRACEFUL_STRING " received.  "
                     "Doing graceful restart");

        /* give the children the signal to die */
        for (i = 0; i < NUM_CHILDS;i++) {
            if ((rv = apr_file_write(pipe_of_death_out, &char_of_death, &one))
                    != APR_SUCCESS) {
                if (APR_STATUS_IS_EINTR(rv)) {
                    continue;
                }
                ap_log_error(APLOG_MARK, APLOG_WARNING, rv, ap_server_conf,
                             "write pipe_of_death");
            }
        }
    }
    else {

        int n = 0;
        for (n = 0; n < NUM_CHILDS; ++n) {
            if (ap_scoreboard_image->servers[n][0].status != SERVER_DEAD &&
                ap_scoreboard_image->parent[n].pid) {
                    unixd_killpg(ap_scoreboard_image->parent[n].pid, SIGTERM);
                    if (CHILD_INFO_TABLE[n].cpu_stopped) {
                        unixd_killpg(ap_scoreboard_image->parent[n].pid, SIGCONT);
                        CHILD_INFO_TABLE[n].cpu_stopped = 0;
                    }
                    CHILD_INFO_TABLE[n].cpu_usage = 0;
            }
        }

        /* Kill 'em off */
        if (unixd_killpg(getpgrp(), SIGHUP) < 0) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "killpg SIGHUP");
        }
        ap_reclaim_child_processes(0); /* Not when just starting up */
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                     "SIGHUP received.  Attempting to restart");
    }

    return 0;
}

/* == allocate an private server config structure == */
static void *peruser_create_config(apr_pool_t *p, server_rec *s)
{
    peruser_server_conf *c =
            (peruser_server_conf *) apr_pcalloc(p, sizeof(peruser_server_conf));

    c->senv = NULL;
    c->missing_senv_reported = 0;

    return c;
}

/* This really should be a post_config hook, but the error log is already
 * redirected by that point, so we need to do this in the open_logs phase.
 */
static int peruser_open_logs(apr_pool_t *p, apr_pool_t *plog,
        apr_pool_t *ptemp, server_rec *s)
{
    apr_status_t rv;

    pconf = p;
    ap_server_conf = s;

    if ((num_listensocks = ap_setup_listeners(ap_server_conf)) < 1) {
        ap_log_error(APLOG_MARK, APLOG_ALERT | APLOG_STARTUP, 0, NULL,
                     "no listening sockets available, shutting down");
        return DONE;
    }

    ap_log_pid(pconf, ap_pid_fname);

    if ((rv = ap_mpm_pod_open(pconf, &pod))) {
        ap_log_error(APLOG_MARK, APLOG_CRIT | APLOG_STARTUP, rv, NULL,
                     "Could not open pipe-of-death.");
        return DONE;
    }

    if ((rv = apr_file_pipe_create(&pipe_of_death_in,
                                   &pipe_of_death_out, pconf))
            != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv,
                     (const server_rec*) ap_server_conf,
                     "apr_file_pipe_create (pipe_of_death)");
        exit(1);
    }

    if ((rv = apr_file_pipe_timeout_set(pipe_of_death_in, 0)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv,
                     (const server_rec*) ap_server_conf,
                     "apr_file_pipe_timeout_set (pipe_of_death)");
        exit(1);
    }

    return OK;
}

static int restart_num = 0;
static int peruser_pre_config(apr_pool_t *p, apr_pool_t *plog,
        apr_pool_t *ptemp)
{
    int no_detach, debug, foreground, i;
    int tmp_server_limit = DEFAULT_SERVER_LIMIT;
    ap_directive_t *pdir;
    apr_status_t rv;
    apr_pool_t *global_pool;
    void *shmem;

    mpm_state = AP_MPMQ_STARTING;

    debug = ap_exists_config_define("DEBUG");

    if (debug) {
        foreground = one_process = 1;
        no_detach = 0;
    }
    else {
        no_detach = ap_exists_config_define("NO_DETACH");
        one_process = ap_exists_config_define("ONE_PROCESS");
        foreground = ap_exists_config_define("FOREGROUND");
    }

    /* sigh, want this only the second time around */
    if (restart_num++ == 1) {
        if (!one_process && !foreground) {
            rv = apr_proc_detach(no_detach ? APR_PROC_DETACH_FOREGROUND
                    : APR_PROC_DETACH_DAEMONIZE);

            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
                             "apr_proc_detach failed");
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        parent_pid = ap_my_pid = getpid();
    }

    unixd_pre_config(ptemp);
    ap_listen_pre_config();
    ap_start_processors = DEFAULT_START_PROCESSORS;
    if (ap_start_processors < 1) ap_start_processors = 1;
    ap_min_processors = DEFAULT_MIN_PROCESSORS;
    ap_min_free_processors = DEFAULT_MIN_FREE_PROCESSORS;
    ap_max_free_processors = DEFAULT_MAX_FREE_PROCESSORS;
    ap_max_processors = DEFAULT_MAX_PROCESSORS;
    if (ap_max_processors < 1) ap_max_processors = 1;
    ap_max_cpu_usage = DEFAULT_MAX_CPU_USAGE;
    if (ap_max_cpu_usage < 1 || ap_max_cpu_usage > 99) ap_max_cpu_usage = 0;
    if (ap_start_processors > ap_max_processors)
        ap_start_processors = ap_max_processors;
    ap_min_multiplexers = DEFAULT_MIN_MULTIPLEXERS;
    ap_max_multiplexers = DEFAULT_MAX_MULTIPLEXERS;
    ap_daemons_limit = server_limit;
    ap_pid_fname = DEFAULT_PIDLOG;
    ap_lock_fname = DEFAULT_LOCKFILE;
    ap_max_requests_per_child = DEFAULT_MAX_REQUESTS_PER_CHILD;
    ap_extended_status = 1;
#ifdef AP_MPM_WANT_SET_MAX_MEM_FREE
    ap_max_mem_free = APR_ALLOCATOR_MAX_FREE_UNLIMITED;
#endif

    expire_timeout = DEFAULT_EXPIRE_TIMEOUT;
    idle_timeout = DEFAULT_IDLE_TIMEOUT;
    multiplexer_idle_timeout = DEFAULT_MULTIPLEXER_IDLE_TIMEOUT;
    processor_wait_timeout = DEFAULT_PROCESSOR_WAIT_TIMEOUT;
    processor_wait_steps = DEFAULT_PROCESSOR_WAIT_STEPS;

    apr_cpystrn(ap_coredump_dir, ap_server_root, sizeof(ap_coredump_dir));

    /* we need to know ServerLimit and ThreadLimit before we start processing
     * the tree because we need to already have allocated child_info_table
     */
    for (pdir = ap_conftree; pdir != NULL; pdir = pdir->next) {
        if (!strcasecmp(pdir->directive, "ServerLimit")) {
            if (atoi(pdir->args) > tmp_server_limit) {
                tmp_server_limit = atoi(pdir->args);
                if (tmp_server_limit > MAX_SERVER_LIMIT) {
                    tmp_server_limit = MAX_SERVER_LIMIT;
                }
            }
        }
    }

    /* We don't want to have to recreate the scoreboard after
     * restarts, so we'll create a global pool and never clean it.
     */
    rv = apr_pool_create(&global_pool, NULL);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
                     "Fatal error: unable to create global pool");
        return rv;
    }

    if (!child_info_image) {
        _DBG("Initializing child_info_table", 0);
        child_info_size = tmp_server_limit * sizeof(child_info_t)
                          + sizeof(apr_size_t);

        rv = apr_shm_create(&child_info_shm, child_info_size, NULL,
                            global_pool);

        /*  if ((rv != APR_SUCCESS) && (rv != APR_ENOTIMPL)) { */
        if (rv != APR_SUCCESS) {
            _DBG("shared memory creation failed", 0);

            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
                         "Unable to create shared memory segment "
                         "(anonymous shared memory failure)");
        }
        else if (rv == APR_ENOTIMPL) {
            _DBG("anonymous shared memory not available", 0);
            /* TODO: make up a filename and do name-based shmem */
        }

        if (rv || !(shmem = apr_shm_baseaddr_get(child_info_shm))) {
            _DBG("apr_shm_baseaddr_get() failed", 0);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        child_info_image = (child_info*) apr_palloc(global_pool,
                                                    sizeof(child_info));
        child_info_image->control = (child_info_control*) shmem;
        shmem += sizeof(child_info_control);
        child_info_image->table = (child_info_t*) shmem;

        for (i = 0; i < tmp_server_limit; i++) {
            memset(&CHILD_INFO_TABLE[i], 0, sizeof(child_info_t));
            CHILD_INFO_TABLE[i].id = i;
        }
    }
    else {
        for (i = 0; i < NUM_CHILDS; i++) {
            if (!is_graceful || (CHILD_INFO_TABLE[i].pid == 0
                && CHILD_INFO_TABLE[i].status == CHILD_STATUS_STANDBY)) {
                memset(&CHILD_INFO_TABLE[i], 0, sizeof(child_info_t));
                CHILD_INFO_TABLE[i].id = i;
            }
        }
    }

    if (!server_env_image) {
        _DBG("Initializing server_environments_table", 0);

        server_env_size = tmp_server_limit * sizeof(server_env_t)
                          + sizeof(apr_size_t);

        rv = apr_shm_create(&server_env_shm, server_env_size, NULL,
                            global_pool);

        if (rv != APR_SUCCESS) {
            _DBG("shared memory creation failed", 0);

            ap_log_error(APLOG_MARK, APLOG_CRIT, rv, NULL,
                         "Unable to create shared memory segment "
                         "(anonymous shared memory failure)");
        }
        else if (rv == APR_ENOTIMPL) {
            _DBG("anonymous shared memory not available", 0);
            /* TODO: make up a filename and do name-based shmem */
        }

        if (rv || !(shmem = apr_shm_baseaddr_get(server_env_shm))) {
            _DBG("apr_shm_baseaddr_get() failed", 0);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        memset(shmem, 0, server_env_size);
        server_env_image = (server_env*) apr_palloc(global_pool,
                                                    sizeof(server_env));
        server_env_image->control = (server_env_control*) shmem;
        shmem += sizeof(server_env_control);
        server_env_image->table = (server_env_t*) shmem;
    }

    _DBG("Clearing server environment table");
    server_env_image->control->num = 0;

    for (i = 0; i < tmp_server_limit; i++) {
        SENV[i].processor_id = -1;
        SENV[i].uid = -1;
        SENV[i].gid = -1;
        SENV[i].chroot = NULL;
        SENV[i].input = -1;
        SENV[i].output = -1;
        SENV[i].error_cgroup = 0;
        SENV[i].error_pass = 0;
    }

    return OK;
}

static int peruser_post_config(apr_pool_t *p, apr_pool_t *plog,
        apr_pool_t *ptemp, server_rec *server_list)
{
    const char *r;
    server_env_t senv;

    /* Retrieve the function from mod_ssl for detecting SSL virtualhosts */
    ssl_server_is_https = (ssl_server_is_https_t) apr_dynamic_fn_retrieve(
                                                         "ssl_server_is_https");

    /* Create the server environment for multiplexers */
    senv.uid = unixd_config.user_id;
    senv.gid = unixd_config.group_id;
    senv.chroot = multiplexer_chroot;
    senv.cgroup = NULL;
    senv.nice_lvl = 0;
    senv.name = "Multiplexer";
    senv.stats_connections = 0;
    senv.stats_requests = 0;
    senv.stats_dropped = 0;
    senv.total_processors = 0;
    senv.idle_processors = 0;
    senv.active_processors = 0;
    senv.total_processes = 0;

    senv.start_processors = ap_start_processors;
    senv.min_processors = ap_min_multiplexers;
    senv.min_free_processors = ap_min_free_processors;
    senv.max_free_processors = ap_max_free_processors;
    senv.max_processors = ap_max_multiplexers;
    senv.max_cpu_usage = 0;

    if (senv.start_processors > senv.max_processors)
        senv.start_processors = senv.max_processors;

    r = child_add(CHILD_TYPE_MULTIPLEXER, CHILD_STATUS_STARTING, p,
                  &senv);

    if (r == NULL) {
        if (multiplexer_senv != NULL) {
            /* first multiplexer */
            multiplexer_senv->total_processors++;
            multiplexer_senv->idle_processors++;
            multiplexer_senv->total_processes++;
            if (multiplexer_senv->total_processors == 1 &&
                multiplexer_senv->start_processors > 1) {
                int x;
                for (x = 1; x < multiplexer_senv->start_processors; ++x)
                    child_clone(multiplexer_senv->processor_id);
                child_info_image->control->new = 1;
                _DBG("Starting %d multiplexers", multiplexer_senv->start_processors);
            }
        }
    } else {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL, r);
        return -1;
    }

    return OK;
}

static int peruser_post_read(request_rec *r)
{
    int retval;

    _DBG("function entered");

    peruser_server_conf *sconf = PERUSER_SERVER_CONF(r->server->module_config);
    child_info_t *processor;

    if (sconf->senv == NULL) {
        _DBG("Server environment not set on virtualhost %s",
             r->server->server_hostname);

        if (sconf->missing_senv_reported == 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                         "Virtualhost %s has no server environment set, "
                         "request will not be honoured.",
                         r->server->server_hostname);
        }

        sconf->missing_senv_reported = 1;

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (CHILD_INFO_TABLE[my_child_num].type == CHILD_TYPE_MULTIPLEXER) {
        processor = &CHILD_INFO_TABLE[sconf->senv->processor_id];
    }
    else {
        processor = &CHILD_INFO_TABLE[r->connection->id];
    }

    if (!strlen(r->the_request)) {
        _DBG("corrupt request. aborting",0);
        return DECLINED;
    }

    if (processor->cpu_stopped && processor->pid) {
        unixd_killpg(processor->pid, SIGCONT);
    }
    processor->cpu_stopped = 0;


    if (processor->sock_fd != AP_PERUSER_THISCHILD) {
        apr_socket_t *sock = NULL;

        apr_os_sock_put(&sock, &processor->sock_fd, r->connection->pool);
        ap_sock_disable_nagle(sock);
        ap_set_module_config(r->connection->conn_config, &core_module, sock);
        _DBG("not the right socket?", 0);
        return OK;
    }

    switch (CHILD_INFO_TABLE[my_child_num].type) {
    case CHILD_TYPE_MULTIPLEXER:
    {
        _DBG("MULTIPLEXER => Determining if request should be passed. "
                "Child Num: %d, dest-child: %d, hostname from server: %s "
                "r->hostname=%s r->the_request=\"%s\"",
                my_child_num, processor->id, r->server->server_hostname,
                r->hostname, r->the_request);

        if (processor->id != my_child_num) {
            if (processor->status == CHILD_STATUS_STANDBY) {
                _DBG("Activating child #%d", processor->id);
                processor->status = CHILD_STATUS_STARTING;
                _DBG("New processor %s started", processor->senv->name);
                // Only start N processors if this is the first processor being started
                if (processor->senv->start_processors > 1
                    && processor->senv->total_processors == 1) {
                    _DBG("Starting %d processors for %s",
                    processor->senv->start_processors, processor->senv->name);
                    int x;
                    for (x = 1; x < processor->senv->start_processors; ++x)
                        child_clone(processor->id);
                }
                child_info_image->control->new = 1;
            }

            _DBG("Passing request.",0);
            retval = pass_request(r, processor);
            if (retval == -1) {
                if (processor->senv) {
                    if (processor->senv->error_pass == 0) {
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                                     "Could not pass request to processor %s "
                                     "(virtualhost %s), "
                                     "request will not be honoured.",
                                     processor->senv->name, r->hostname);
                    }
                    processor->senv->error_pass = 1;
                    processor->senv->stats_dropped++;
                }

                return HTTP_SERVICE_UNAVAILABLE;
            }
            else if (retval == -2) {
                /* Request too big - protecting from segfault */
                return HTTP_REQUEST_ENTITY_TOO_LARGE;
            }
            else {
                processor->senv->error_pass = 0;
            }

            _DBG("doing longjmp",0);

            longjmp(CHILD_INFO_TABLE[my_child_num].jmpbuffer, 1);

            _DBG("request declined at our site",0);

            return DECLINED;
        }

        _DBG("The server is assigned to the multiplexer! Dropping request");

        return DECLINED;
    }
    case CHILD_TYPE_PROCESSOR:
    case CHILD_TYPE_WORKER:
    {
        if (sconf->senv != CHILD_INFO_TABLE[my_child_num].senv) {
            _DBG("request not for this worker - passing it back to the "
                 "multiplexer");

            child_info_t *multiplexer = &CHILD_INFO_TABLE[multiplexer_senv->
                                                          processor_id];

            if (pass_request(r, multiplexer) == -1) {
                if (multiplexer->senv->error_pass == 0) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf,
                                 "Could not pass request to multiplexer, "
                                 "request will not be honoured (%s).",
                                 r->hostname);
                }

                multiplexer->senv->error_pass = 1;
                multiplexer->senv->stats_dropped++;

                return HTTP_SERVICE_UNAVAILABLE;
            }
            else {
                multiplexer->senv->error_pass = 0;
            }
            _DBG("doing longjmp",0);

            longjmp(CHILD_INFO_TABLE[my_child_num].jmpbuffer, 1);

            return DECLINED;
        }

        _DBG("%s %d", child_type_string(CHILD_INFO_TABLE[my_child_num].type),
             my_child_num);

        _DBG("request for %s / (server %s) seems to be for us", r->hostname,
             r->server->server_hostname);

        if (server_env_cleanup) {
            int i;
            int input = sconf->senv->input;
            int output = sconf->senv->output;

            _DBG("performing handle cleanup");
            for (i = 0; i < NUM_SENV; i++) {
                if (&SENV[i] == multiplexer_senv) {
                    continue;
                }

                if (SENV[i].input > 0 && SENV[i].input != input) {
                    int retval = close(SENV[i].input);
                    if (retval < 0) {
                        ap_log_error(APLOG_MARK, APLOG_WARNING, errno,
                                     ap_server_conf, "close(%d) failed",
                                     SENV[i].input);
                    }
                }

                if (SENV[i].output > 0 && SENV[i].output != output) {
                    int retval = close(SENV[i].output);
                    if (retval < 0) {
                        ap_log_error(APLOG_MARK, APLOG_WARNING, errno,
                                     ap_server_conf, "close(%d) failed",
                                     SENV[i].output);
                    }
                }
            }
            server_env_cleanup = 0;
        }

        CHILD_INFO_TABLE[my_child_num].senv->stats_requests++;

        return OK;
    }
    default:
    {
        _DBG("unspecified child type %d in %d, dropping request",
             CHILD_INFO_TABLE[my_child_num].type, my_child_num);
        return DECLINED;
    }
    }

    _DBG("THIS POINT SHOULD NOT BE REACHED!",0);

    return OK;
}

int senv_active_cmp(const void *a, const void *b)
{
    _DBG("CMP %d %d", *(int *) a,*(int *) b);

    return active_env_processors(*(int *) a)
            < active_env_processors(*(int *) b);
}

static int peruser_status_hook(request_rec *r, int flags)
{
    int x;
    child_info_t *child;
    server_env_t *senv;

    if (flags & AP_STATUS_SHORT) {
        return OK;
    }

    if (flags & AP_STATUS_PERUSER_STATS) {
        int *sorted_senv;
        int i;
        sorted_senv = (int *) apr_palloc(r->pool, NUM_SENV * sizeof(int));

        if (!sorted_senv) {
            ap_rputs("peruser_status_hook(): Out of memory", r);
            return OK;
        }

        /* Initial senv table */
        for (i = 0; i < NUM_SENV; i++) {
            sorted_senv[i] = i;
        }

        /* sort env by number of processors */
        qsort(sorted_senv, NUM_SENV, sizeof(int), senv_active_cmp);

        ap_rputs("<h3>Processors statistics:</h3>"
                 "<table width=700 border=\"1\" cellpadding=4 cellspacing=0><tr><th>ID</th><th>Environment</th>"
                 "<th>Active</th><th>Idle</th><th>Max</th>"
                 "<th>Connections</th><th>Requests</th><th>Dropped (503)</th>"
                 "<th>Avail</th></tr>", r);

        /* just a mockup to see what data will be useful here will put code
         * later, yes I know we need to iterate ON ENV[] NUM_ENV times */
        for (x = 0; x < NUM_SENV; x++) {
            senv = &SENV[sorted_senv[x]];
            if (senv == NULL) {
                continue;
            }

            ap_rprintf(r, "<tr><td>%d</td><td nowrap>%s</td><td>%d</td><td>%d</td><td>%d</td>"
                       "<td>%d</td><td>%d</td><td>%d</td><td>%d%%</td></tr>",
                       senv->id,
                       senv->name == NULL ? "" : senv->name,
                       active_env_processors(sorted_senv[x]),
                       senv->idle_processors,
                       senv->max_processors, senv->stats_connections,
                       senv->stats_requests, senv->stats_dropped,
                       senv->availability);
        }
        ap_rputs("</table><tr/>", r);

        ap_rputs("<hr/><table>"
            "</table><hr/>", r);
    }
    else {
        ap_rputs("<hr>\n", r);
        ap_rputs("<h3>peruser status</h3>\n", r);
        ap_rputs("<table border=\"1\" cellpadding=4 cellspacing=0>\n", r);
        ap_rputs("<tr><th>ID</th><th>PID</th><th>STATUS</th><th>SB STATUS</th>"
                 "<th>Type</th><th>Processor</th><th>Active</th><th>Idle</th><th>Max</th>"
                 "<th>AVAIL</th><th>CPU Stopped</th><th>CPU Usage</th><th>CPU Total</th></tr>\n", r);

        for (x = 0; x < NUM_CHILDS; x++) {
            child = &CHILD_INFO_TABLE[x];
            senv = child->senv;
            if (senv == NULL) continue;
            ap_rprintf(r,
                       "<tr><td>%3d</td><td>%5d</td><td>%8s</td><td>%8s</td>"
                       "<td>%12s</td><td nowrap>%48s</td><td>%d</td><td>%d</td><td>%d</td>"
                       "<td>%3d%%</td><td>%d</td><td>%.1f%%</td><td>%.1f%%</td></tr>\n",
                       child->id, child->pid,
                       child_status_string(child->status),
                       scoreboard_status_string(SCOREBOARD_STATUS(x)),
                       child_type_string(child->type),
                       senv == NULL ? NULL : (senv->name == NULL ? "" :
                                              senv->name),
                       senv == NULL ? 0 : senv->active_processors,
                       senv == NULL ? 0 : senv->idle_processors,
                       senv == NULL ? 0 : senv->max_processors,
                       senv == NULL ? 0 : senv->availability,
                       child->cpu_stopped,
                       child->cpu_usage,
                       child->senv->total_cpu_usage);
        }
        ap_rputs("</table><hr/>\n", r);

    }

    return OK;
}

static void peruser_hooks(apr_pool_t *p)
{
    /* The peruser open_logs phase must run before the core's, or stderr
     * will be redirected to a file, and the messages won't print to the
     * console.
     */
    static const char * const aszSucc[] = { "core.c", NULL };

#ifdef AUX3
    (void) set42sig();
#endif

    ap_hook_open_logs(peruser_open_logs, NULL, aszSucc, APR_HOOK_MIDDLE);
    ap_hook_pre_config(peruser_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(peruser_post_config, NULL, NULL, APR_HOOK_MIDDLE);

    /* Both of these must be run absolutely first.  If this request isn't for
     * this server then we need to forward it to the proper child.  No sense
     * tying up this server running more post_read request hooks if it is
     * just going to be forwarded along.  The process_connection hook allows
     * peruser to receive the passed request correctly, by automatically
     * filling in the core_input_filter's ctx pointer.
     */
    ap_hook_post_read_request(peruser_post_read, NULL, NULL,
                              APR_HOOK_REALLY_FIRST);
    ap_hook_process_connection(peruser_process_connection, NULL, NULL,
                               APR_HOOK_REALLY_FIRST);

    APR_OPTIONAL_HOOK(ap, status_hook, peruser_status_hook, NULL, NULL,
                      APR_HOOK_MIDDLE);
}

void senv_init(server_env_t * senv)
{
    senv->nice_lvl = 0;
    senv->chroot = NULL;
    senv->cgroup = NULL;
    senv->start_processors = ap_start_processors;
    senv->min_processors = ap_min_processors;
    senv->min_free_processors = ap_min_free_processors;
    senv->max_free_processors = ap_max_free_processors;
    senv->max_processors = ap_max_processors;
    senv->max_cpu_usage = ap_max_cpu_usage;
    if (senv->start_processors > senv->max_processors)
        senv->start_processors = senv->max_processors;
    senv->error_cgroup = 0;
    senv->error_pass = 0;
    senv->stats_connections = 0;
    senv->stats_requests = 0;
    senv->stats_dropped = 0;
    senv->total_processors = 0;
    senv->idle_processors = 0;
    senv->active_processors = 0;
    senv->total_processes = 0;
}

static const char *cf_Processor(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *user_name = NULL, *group_name = NULL, *directive;
    server_env_t senv;
    ap_directive_t *current;

    const char *endp = ap_strrchr_c(arg, '>');

    if (endp == NULL) {
        return apr_psprintf(cmd->temp_pool,
                            "Error: Directive %s> missing closing '>'",
                            cmd->cmd->name);
    }

    arg = apr_pstrndup(cmd->pool, arg, endp - arg);

    if (!arg) {
        return apr_psprintf(cmd->temp_pool,
                            "Error: %s> must specify a processor name",
                            cmd->cmd->name);
    }

    senv.name = ap_getword_conf(cmd->pool, &arg);
    _DBG("processor_name: %s", senv.name);

    if (strlen(senv.name) == 0) {
        return apr_psprintf(cmd->temp_pool,
                            "Error: Directive %s> takes one argument",
                            cmd->cmd->name);
    }

    server_env_t *old_senv = find_senv_by_name(senv.name);

    if (old_senv) {
        return apr_psprintf(cmd->temp_pool,
                            "Error: Processor %s already defined", senv.name);
    }

    senv_init(&senv);

    current = cmd->directive->first_child;

    int proc_temp = 0;
    for (; current != NULL; current = current->next) {
        directive = current->directive;

        if (!strcasecmp(directive, "user")) {
            user_name = current->args;
        }
        else if (!strcasecmp(directive, "group")) {
            group_name = current->args;
        }
        else if (!strcasecmp(directive, "chroot")) {
            senv.chroot = ap_getword_conf(cmd->pool, &current->args);
        }
        else if (!strcasecmp(directive, "nicelevel")) {
            senv.nice_lvl = atoi(current->args);
        }
        else if (!strcasecmp(directive, "maxcpuusage")) {
            proc_temp = atoi(current->args);
 
            if (proc_temp < 0 || proc_temp > 99) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require MaxCPUUsage between 0 AND 100,"
                             "setting to 0");
                proc_temp = 0;
            }
    
            senv.max_cpu_usage = proc_temp;
        }
        else if (!strcasecmp(directive, "maxprocessors")) {
            proc_temp = atoi(current->args);

            if (proc_temp < 1) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require MaxProcessors > 0,"
                             "setting to 1");
                proc_temp = 1;
            }

            senv.max_processors = proc_temp;
        }
        else if (!strcasecmp(directive, "minprocessors")) {
            proc_temp = atoi(current->args);

            if (proc_temp < 0) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require MinProcessors >= 0,"
                             "setting to 0");
                proc_temp = 0;
            }

            senv.min_processors = proc_temp;
        }
        else if (!strcasecmp(directive, "startprocessors")) {
            proc_temp = atoi(current->args);
                      
            if (proc_temp < 1) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require StartProcessors > 0,"
                             "setting to 1");
                proc_temp = 1;
            }
    
            senv.start_processors = proc_temp;
        }
        else if (!strcasecmp(directive, "minspareprocessors")) {
            proc_temp = atoi(current->args);

            if (proc_temp < 0) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require MinSpareProcessors >= 0,"
                             "setting to 0");
                proc_temp = 0;
            }

            senv.min_free_processors = proc_temp;
        }
        else if (!strcasecmp(directive, "maxspareprocessors")) {
            proc_temp = atoi(current->args);

            if (proc_temp < 0) {
                ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                             "WARNING: Require MaxSpareProcessors >= 0,"
                             "setting to 0");
                proc_temp = 0;
            }

            senv.max_free_processors = proc_temp;
        }
        else if (!strcasecmp(directive, "cgroup")) {
            senv.cgroup = ap_getword_conf(cmd->pool, &current->args);
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "Unknown directive %s in %s>", directive,
                         cmd->cmd->name);
        }
    }

    if (user_name == NULL || group_name == NULL) {
        return apr_psprintf(cmd->temp_pool,
                            "Error: User or Group must be set in %s>",
                            cmd->cmd->name);
    }

    senv.uid = ap_uname2id(user_name);
    senv.gid = ap_gname2id(group_name);

    _DBG("name=%s user=%s:%d group=%s:%d chroot=%s nice_lvl=%d",
         senv.name, user_name, senv.uid, group_name, senv.gid, senv.chroot,
         senv.nice_lvl);

    _DBG("start_processors=%d max_cpu_usage=%d min_processors=%d min_free_processors=%d max_spare_processors=%d "
         "max_processors=%d", senv.start_processors, senv.max_cpu_usage, senv.min_processors, senv.min_free_processors,
         senv.max_free_processors, senv.max_processors);

    return child_add(CHILD_TYPE_PROCESSOR, CHILD_STATUS_STANDBY, cmd->pool,
                     &senv);
}

static const char *cf_Processor_depr(cmd_parms *cmd, void *dummy,
                                     const char *user_name,
                                     const char *group_name, const char *chroot)
{
    return NULL;
}

/* we define an Multiplexer child w/ specific uid/gid */
static const char *cf_Multiplexer(cmd_parms *cmd, void *dummy,
                                  const char *user_name, const char *group_name,
                                  const char *chroot)
{
    static short depr_warned = 0;

    if (depr_warned == 0) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Multiplexer directive is deprecated."
                     "Multiplexer user and group is set by User and Group"
                     "directives.");

        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "To set multiplexer chroot, "
                     "please use MultiplexerChroot.");

        depr_warned = 1;
    }

    if (chroot) {
        if (!ap_is_directory(cmd->pool, chroot)) {
            return apr_psprintf(cmd->pool, "Error:multiplexer chroot directory"
                                "[%s] does not exist", chroot);
        }

        multiplexer_chroot = chroot;
        _DBG("Setting multiplexer chroot to %s", chroot);
    }

    return NULL;
}

static const char* cf_MultiplexerChroot(cmd_parms *cmd, void *dummy,
                                        const char *path)
{
    multiplexer_chroot = path;

    if (path && !ap_is_directory(cmd->pool, path))
        return apr_psprintf(cmd->pool, "Error: multiplexer chroot directory"
                            "[%s] does not exist", path);

    _DBG("setting multiplexer chroot to %s", path);

    return NULL;
}

static const char* cf_ServerEnvironment(cmd_parms *cmd, void *dummy,
        const char *name, const char * group_name, const char * chroot)
{
    peruser_server_conf *sconf = PERUSER_SERVER_CONF(cmd->server->module_config);
    server_env_t senv;
    char * processor_name, *tmp;

    _DBG("function entered", 0);

    /* name of processor env */
    processor_name = name;

    if (group_name != NULL || chroot != NULL) {
        /* deprecated ServerEnvironment user group chroot syntax
         * we create simple server env based on user/group/chroot only
         */
        processor_name = apr_pstrcat(cmd->pool, name, "_", group_name, "_",
                                     chroot, NULL);

        /* search for previous default server env */
        sconf->senv = find_senv_by_name(processor_name);

        if (!sconf->senv) {
            senv_init(&senv);
            senv.uid = ap_uname2id(name);
            senv.gid = ap_gname2id(group_name);
            senv.chroot = chroot;
            senv.name = processor_name;

            tmp = child_add(CHILD_TYPE_PROCESSOR, CHILD_STATUS_STANDBY,
                            cmd->pool, &senv);
            /* error handling in case this child can't be created */
            if (tmp) {
                return tmp;
            }
        }
    }

    /* use predefined processor environment or default named "user_group_chroot" */
    if (sconf->senv == NULL) {
        sconf->senv = find_senv_by_name(processor_name);
    }

    if (sconf->senv == NULL) {
        return apr_psprintf(cmd->pool, "Error: Processor %s not defined", name);
    }

    _DBG("user=%d group=%d chroot=%s numchilds=%d",
         sconf->senv->uid, sconf->senv->gid, sconf->senv->chroot, NUM_CHILDS);

    return NULL;
}

static const char *set_min_free_servers(cmd_parms *cmd, void *dummy,
                                        const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    ap_min_free_processors = atoi(arg);
    if (ap_min_free_processors <= 0) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: detected MinSpareServers set to non-positive.");
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "Resetting to 1 to avoid almost certain Apache failure.");
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "Please read the documentation.");
        ap_min_free_processors = 1;
    }

    return NULL;
}

static const char *set_max_clients(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    ap_daemons_limit = atoi(arg);

    if (ap_daemons_limit > server_limit) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: MaxClients of %d exceeds ServerLimit value "
                     "of %d servers,", ap_daemons_limit, server_limit);
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     " lowering MaxClients to %d.  To increase, please "
                     "see the ServerLimit", server_limit);
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL, " directive.");
        ap_daemons_limit = server_limit;
    }
    else if (ap_daemons_limit < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MaxClients > 0, setting to 1");
        ap_daemons_limit = 1;
    }

    return NULL;
}

static const char *set_start_processors(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    peruser_server_conf *sconf;
    int start_procs;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    start_procs = atoi(arg);

    if (start_procs < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require StartProcessors > 0, setting to 1");
        start_procs = 1;
    }
 
    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);
    
        if (sconf->senv != NULL) {
            sconf->senv->start_processors = start_procs;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "WARNING: StartProcessors must be set"
                         "after ServerEnvironment to take effect");
        }
    } else {
        ap_start_processors = start_procs;
    }

    return NULL;
}

static const char *set_min_processors(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    peruser_server_conf *sconf;
    int min_procs;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    min_procs = atoi(arg);

    if (min_procs < 0) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MinProcessors >= 0, setting to 0");
        min_procs = 0;
    }

    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);

        if (sconf->senv != NULL) {
            sconf->senv->min_processors = min_procs;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "WARNING: MinProcessors must be set"
                         "after ServerEnvironment to take effect");
        }
    } else {
        ap_min_processors = min_procs;
    }

    return NULL;
}

static const char *set_min_free_processors(cmd_parms *cmd, void *dummy,
                                           const char *arg)
{
    peruser_server_conf *sconf;
    int min_free_procs;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    min_free_procs = atoi(arg);

    if (min_free_procs < 0) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MinSpareProcessors >= 0, setting to 0");
        min_free_procs = 0;
    }

    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);
        if (sconf->senv != NULL) {
            sconf->senv->min_free_processors = min_free_procs;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "WARNING: MinSpareProcessors must be set"
                         "after ServerEnvironment to take effect");
        }
    } else {
        ap_min_free_processors = min_free_procs;
    }

    return NULL;
}

static const char *set_max_free_processors(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    peruser_server_conf *sconf;
    int max_free_procs;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    max_free_procs = atoi(arg);

    if (max_free_procs < 0) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MaxSpareProcessors >= 0, setting to 0");
        max_free_procs = 0;
    }

    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);

        if (sconf != NULL) {
            sconf->senv->max_free_processors = max_free_procs;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "WARNING: MaxSpareProcessors must be set"
                         "after ServerEnvironment to take effect");
        }
    } else {
        ap_max_free_processors = max_free_procs;
    }

    return NULL;
}

static const char *set_max_cpu_usage(cmd_parms *cmd, void *dummy,
                                      const char *arg)
{
    peruser_server_conf *sconf;
    int max_cpu;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    max_cpu = atoi(arg);

    if (max_cpu < 0 || max_cpu > 99) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MaxCPUUsage between 0 AND 100, setting to 0");
        max_cpu = 1;
    }

    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);
        if (sconf->senv != NULL) {
            sconf->senv->max_cpu_usage = max_cpu;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                                     "WARNING: MaxCPUUsage must be set"
                                     "after ServerEnvironment to take effect");
        }
    } else {
        ap_max_cpu_usage = max_cpu;
    }

    return NULL;
}

static const char *set_max_processors(cmd_parms *cmd, void *dummy,
                                      const char *arg)
{
    peruser_server_conf *sconf;
    int max_procs;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    max_procs = atoi(arg);

    if (max_procs < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MaxProcessors > 0, setting to 1");
        max_procs = 1;
    }

    if (ap_check_cmd_context(cmd, NOT_IN_VIRTUALHOST) != NULL) {
        sconf = PERUSER_SERVER_CONF(cmd->server->module_config);
        if (sconf->senv != NULL) {
            sconf->senv->max_processors = max_procs;
        } else {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                                     "WARNING: MaxProcessors must be set"
                                     "after ServerEnvironment to take effect");
        }
    } else {
        ap_max_processors = max_procs;
    }

    return NULL;
}

static const char *set_min_multiplexers(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    int min_multiplexers;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    min_multiplexers = atoi(arg);

    if (min_multiplexers < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MinMultiplexers > 0, setting to 1");
        min_multiplexers = 1;
    }

    ap_min_multiplexers = min_multiplexers;

    return NULL;
}

static const char *set_max_multiplexers(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    int max_multiplexers;
    const char *err = ap_check_cmd_context(cmd, NOT_IN_DIR_LOC_FILE
                                           | NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    max_multiplexers = atoi(arg);

    if (max_multiplexers < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require MaxMultiplexers > 0, setting to 1");
        max_multiplexers = 1;
    }

    ap_max_multiplexers = max_multiplexers;

    return NULL;
}

static const char *set_server_limit(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    int tmp_server_limit;

    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    tmp_server_limit = atoi(arg);

    /* you cannot change ServerLimit across a restart; ignore
     * any such attempts
     */
    if (first_server_limit && tmp_server_limit != server_limit) {
        /* how do we log a message?  the error log is a bit bucket at this
         * point; we'll just have to set a flag so that ap_mpm_run()
         * logs a warning later
         */
        changed_limit_at_restart = 1;
        return NULL;
    }

    server_limit = tmp_server_limit;

    if (server_limit > MAX_SERVER_LIMIT) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: ServerLimit of %d exceeds compile time limit "
                         "of %d servers,", server_limit, MAX_SERVER_LIMIT);
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     " lowering ServerLimit to %d.", MAX_SERVER_LIMIT);
        server_limit = MAX_SERVER_LIMIT;
    } else if (server_limit < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "WARNING: Require ServerLimit > 0, setting to 1");
        server_limit = 1;
    }

    return NULL;
}

static const char *set_expire_timeout(cmd_parms *cmd, void *dummy,
                                      const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    expire_timeout = atoi(arg);

    return NULL;
}

static const char *set_idle_timeout(cmd_parms *cmd, void *dummy,
                                    const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    idle_timeout = atoi(arg);

    return NULL;
}

static const char *set_multiplexer_idle_timeout(cmd_parms *cmd, void *dummy,
        const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    multiplexer_idle_timeout = atoi(arg);

    return NULL;
}

static const char *set_processor_wait_timeout(cmd_parms *cmd, void *dummy,
                                              const char *timeout,
                                              const char *steps)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);

    if (err != NULL) {
        return err;
    }

    processor_wait_timeout = atoi(timeout);

    if (steps != NULL) {
        int steps_tmp = atoi(steps);

        if (steps_tmp < 1) {
            ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                         "WARNING: Require ProcessorWaitTimeout steps > 0,"
                         "setting to 1");
            steps_tmp = 1;
        }

        processor_wait_steps = steps_tmp;
    }

    return NULL;
}

static const command_rec peruser_cmds[] = { UNIX_DAEMON_COMMANDS,
LISTEN_COMMANDS,
AP_INIT_TAKE1("MinSpareProcessors", set_min_free_processors, NULL, RSRC_CONF,
              "Minimum number of idle children, to handle request spikes"),
AP_INIT_TAKE1("MinSpareServers", set_min_free_servers, NULL, RSRC_CONF,
              "Minimum number of idle children, to handle request spikes"),
AP_INIT_TAKE1("MaxSpareProcessors", set_max_free_processors, NULL, RSRC_CONF,
              "Maximum number of idle children, 0 to disable"),
AP_INIT_TAKE1("MaxClients", set_max_clients, NULL, RSRC_CONF,
              "Maximum number of children alive at the same time"),
AP_INIT_TAKE1("StartProcessors", set_start_processors, NULL, RSRC_CONF,
              "Number of child processors per vhost at startup"),
AP_INIT_TAKE1("MinProcessors", set_min_processors, NULL, RSRC_CONF,
              "Minimum number of processors per vhost"),
AP_INIT_TAKE1("MaxProcessors", set_max_processors, NULL, RSRC_CONF,
              "Maximum number of processors per vhost"),
AP_INIT_TAKE1("MaxCPUUsage", set_max_cpu_usage, NULL, RSRC_CONF,
              "Maximum CPU usage per active processor"),
AP_INIT_TAKE1("MinMultiplexers", set_min_multiplexers, NULL, RSRC_CONF,
              "Minimum number of multiplexers the server can have")        ,
AP_INIT_TAKE1("MaxMultiplexers", set_max_multiplexers, NULL, RSRC_CONF,
              "Maximum number of multiplexers the server can have"),
AP_INIT_TAKE1("ServerLimit", set_server_limit, NULL, RSRC_CONF,
              "Maximum value of MaxClients for this run of Apache"),
AP_INIT_TAKE1("ExpireTimeout", set_expire_timeout, NULL, RSRC_CONF,
              "Maximum time a child can live, 0 to disable"),
AP_INIT_TAKE1("IdleTimeout", set_idle_timeout, NULL, RSRC_CONF,
              "Maximum time before a child is killed after being idle,"
              "0 to disable"),
AP_INIT_TAKE1("MultiplexerIdleTimeout", set_multiplexer_idle_timeout, NULL,
              RSRC_CONF, "Maximum time before a multiplexer is killed after"
              " being idle, 0 to disable"),
AP_INIT_TAKE12("ProcessorWaitTimeout", set_processor_wait_timeout, NULL,
               RSRC_CONF, "Maximum time a multiplexer waits for the processor"
               " if it is busy"),
AP_INIT_TAKE23("Multiplexer", cf_Multiplexer, NULL, RSRC_CONF,
              "Specify an Multiplexer Child configuration."),
AP_INIT_RAW_ARGS("<Processor", cf_Processor, NULL, RSRC_CONF,
              "Specify settings for processor."),
AP_INIT_TAKE23("Processor", cf_Processor_depr, NULL, RSRC_CONF,
              "A dummy directive for backwards compatibility"),
AP_INIT_TAKE123("ServerEnvironment", cf_ServerEnvironment, NULL, RSRC_CONF,
              "Specify the server environment for this virtual host."),
AP_INIT_TAKE1("MultiplexerChroot", cf_MultiplexerChroot, NULL, RSRC_CONF,
              "Specify the multiplexer chroot path for multiplexer"),
{ NULL }
};

module AP_MODULE_DECLARE_DATA mpm_peruser_module = {
    MPM20_MODULE_STUFF,
    ap_mpm_rewrite_args,    /* hook to run before apache parses args */
    NULL,                   /* create per-directory config structure */
    NULL,                   /* merge per-directory config structures */
    peruser_create_config,  /* create per-server config structure */
    NULL,                   /* merge per-server config structures */
    peruser_cmds,           /* command apr_table_t */
    peruser_hooks,          /* register hooks */
};
