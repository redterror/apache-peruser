#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <fcntl.h>

#ifndef _PATH_DEVNULL
# define _PATH_DEVNULL		"/dev/null"
#endif /* !_PATH_DEVNULL */


static kvm_t *cpu_usage_kd;
static struct kinfo_proc *cpu_usage_kp;
static int cpu_usage_total = 0;
static int cpu_usage_status = 0;

static int cpu_usage_init()
{
    if (cpu_usage_status) return 0;
    cpu_usage_kd = kvm_open(NULL, _PATH_DEVNULL, NULL, O_RDONLY, NULL);
    if (cpu_usage_kd == NULL) return -1;
    cpu_usage_status = 1;
    return 0;
}

void cpu_usage_finish()
{
    if (cpu_usage_status) {
        kvm_close(cpu_usage_kd);
        cpu_usage_status = 0;
    }
}

void cpu_usage_refresh()
{
    if (cpu_usage_init() == 0) {
        cpu_usage_kp = kvm_getprocs(cpu_usage_kd, KERN_PROC_ALL, 0, &cpu_usage_total);
    }
}

double cpu_usage_pid(pid_t pid)
{
    double ret = 0;
    int i;
    struct kinfo_proc *pp;
    if (cpu_usage_kp && cpu_usage_total > 0) {
        ret = 0;
        pp = NULL;
        for (pp = cpu_usage_kp, i = 0; i < cpu_usage_total; pp++, i++) {
            if (pp->ki_pgid == pid)
                ret += (100.0 * pp->ki_pctcpu / FSCALE);
        }
    }
    return ret; 
}


