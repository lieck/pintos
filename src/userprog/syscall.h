#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "userprog/process.h"

void syscall_init(void);

// p1-process control syscalls
int sys_practice(int i);
double sys_compute_e(int n);
void sys_halt(void);
void sys_exit(int status);
pid_t sys_exec(const char* cmd_line);
int sys_wait(pid_t pid);

// p1-file operation syscalls
int sys_create(const char* file, unsigned initial_size);
int sys_remove(const char* file);
int sys_open(const char* file);
int sys_filesize(int fd);
int sys_read(int fd, void* buffer, unsigned size);
int sys_write(int fd, const void* buffer, unsigned size);
void sys_seek(int fd, unsigned position);
unsigned sys_tell(int fd);
void sys_close(int fd);

/* p3 subfilesys */
bool sys_isdir(const char* dir);
int sys_inumber(int fd);
bool sys_readdir(int fd, char* name);
bool sys_chdir(const char* dir);
bool sys_mkdir(const char* dir);

#endif /* userprog/syscall.h */
