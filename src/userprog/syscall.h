#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);


// file
int sys_create(const char *file, unsigned initial_size);
int sys_remove(const char *file);
int sys_open(const char *file);
int sys_filesize (int fd);
int sys_read (int fd, void *buffer, unsigned size);
int sys_write (int fd, const void *buffer, unsigned size);
void sys_seek (int fd, unsigned position);
unsigned sys_tell(int fd);
void sys_close (int fd);

#endif /* userprog/syscall.h */
