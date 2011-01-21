#ifndef APACHE_MPM_CPU_USAGE_H
#define APACHE_MPM_CPU_USAGE_H

void cpu_usage_finish();
void cpu_usage_refresh();
double cpu_usage_pid(pid_t pid);

#endif

