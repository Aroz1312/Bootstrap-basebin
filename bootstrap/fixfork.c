
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <util.h>
#include <signal.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>

#include "common.h"




#define printf_putchar(x) do{int l=strlen(buffer);if(l<bufsize)buffer[l]=x;}while(0)

int _vsnprintf(char* buffer, int bufsize, const char * __restrict format, va_list vl) {
    bool special = false;
    
    memset(buffer,0,bufsize);
    
    while (*format) {
        if (special) {
            switch (*format) {
                case 'x':
                case 'p': {
                    // Pointer
                    printf_putchar('0');
                    printf_putchar('x');
                    
                    uintptr_t ptr = va_arg(vl, uintptr_t);
                    bool didWrite = false;
                    for (int i = 7; i >= 0; i--) {
                        uint8_t cur = (ptr >> (i * 8)) & 0xFF;
                        char first = cur >> 4;
                        if (first >= 0 && first <= 9) {
                            first = first + '0';
                        } else {
                            first = (first - 0xA) + 'A';
                        }
                        
                        char second = cur & 0xF;
                        if (second >= 0 && second <= 9) {
                            second = second + '0';
                        } else {
                            second = (second - 0xA) + 'A';
                        }
                        
                        if (didWrite || cur) {
                            if (didWrite || first != '0') {
                                printf_putchar(first);
                            }
                            
                            printf_putchar(second);
                            didWrite = true;
                        }
                    }
                    
                    if (!didWrite) {
                        printf_putchar('0');
                    }
                    break;
                }

                case 'd': {
                    int i = va_arg(vl, int);
                    #define INT_DIGITS 32
                    char buf[INT_DIGITS + 2]={0};
                    char *p = buf + INT_DIGITS + 1;    /* points to terminating '\0' */
                    if (i >= 0) {
                        do {
                        *--p = '0' + (i % 10);
                        i /= 10;
                        } while (i != 0);
                    }
                    else {            /* i < 0 */
                        do {
                        *--p = '0' - (i % 10);
                        i /= 10;
                        } while (i != 0);
                        *--p = '-';
                    }
                    for(int i=0; i<strlen(p); i++) {
                        printf_putchar(p[i]);
                    }
                    break;
                }
                    
                case 'u': {
                    unsigned int i = va_arg(vl, unsigned int);
                    #define INT_DIGITS 32
                    char buf[INT_DIGITS + 2]={0};
                    char *p = buf + INT_DIGITS + 1;    /* points to terminating '\0' */
                    do {
                      *--p = '0' + (i % 10);
                      i /= 10;
                    } while (i != 0);
                    for(int i=0; i<strlen(p); i++) {
                        printf_putchar(p[i]);
                    }
                    break;
                }
                    
                case 's': {
                    const char *str = va_arg(vl, const char*);
                    if (str == NULL) {
                        str = "<NULL>";
                    }
                    
                    while (*str) {
                        printf_putchar(*str++);
                    }
                    break;
                }
                    
                case 'c':
                    printf_putchar(va_arg(vl, int));
                    break;
                    
                case 'l':
                    // Prefix, ignore
                    format++;
                    continue;
                    
                case '%':
                    printf_putchar(*format);
                    break;
                    
                default:
                    printf_putchar('%');
                    printf_putchar(*format);
                    break;
            }
            
            special = false;
        } else {
            if (*format == '%') {
                special = true;
            } else {
                printf_putchar(*format);
            }
        }
        
        format++;
    }
    
    return 0; // Not up to spec, but who uses the return value of (v)printf anyway?
}

int _snprintf(char* buffer, int bufsize, const char * __restrict format, ...) {
    va_list vl;
    va_start(vl, format);
    
    int res = _vsnprintf(buffer, bufsize, format, vl);
    
    va_end(vl);
    
    return res;
}


#define forklog(...)	do {\
char buf[1024];\
_snprintf(buf,sizeof(buf),__VA_ARGS__);\
write(STDERR_FILENO,buf,strlen(buf));\
write(STDERR_FILENO,"\n",1);\
fsync(STDERR_FILENO);\
} while(0)



void showvm(task_port_t task, uint64_t start, uint64_t size)
{
    vm_size_t region_size=0;
    vm_address_t region_base = start;
    natural_t depth = 1;
    
    while((region_base+region_size) < (start+size)) {
        region_base += region_size;
        
        struct vm_region_submap_info_64 info={0};
        mach_msg_type_number_t info_cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        
        kern_return_t kr = vm_region_recurse_64(task, &region_base, &region_size,
                                          &depth, (vm_region_info_t)&info, &info_cnt);
        
        if(kr != KERN_SUCCESS) {
            forklog("[%d] vm_region failed on %p, %x:%s", getpid(), (void*)region_base, kr, mach_error_string(kr));
            break;
        }
        
        forklog("[%d] found region %p %lx [%d/%d], %x/%x, %d\n", getpid(), (void*)region_base, region_size, info.is_submap, depth, info.protection, info.max_protection, info.user_tag);

        if(info.is_submap) {
            region_size=0;
            depth++;
            continue;
        }
        
    } 
}



__attribute__((noinline, naked)) volatile kern_return_t _mach_vm_protect(mach_port_name_t target, mach_vm_address_t address, mach_vm_size_t size, boolean_t set_maximum, vm_prot_t new_protection)
{
#if __arm64__
    __asm("mov x16, #0xFFFFFFFFFFFFFFF2");
    __asm("svc 0x80");
    __asm("ret");
#else
    __asm(".intel_syntax noprefix; \
           mov rax, 0xFFFFFFFFFFFFFFF2; \
           syscall; \
           ret");
#endif
}

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/syslimits.h>

uint64_t textaddr=0;
uint64_t textsize=0;
uint64_t vmSpaceSize=0;

void forkfix(const char* tag, bool flag, bool child)
{
	kern_return_t kr = -1;
	mach_port_t task = task_self_trap();//mach_port_deallocate for task_self_trap()

    // if(flag) {
    //     int count=0;
    //     thread_act_array_t list;
    //     assert(task_threads(mach_task_self(), &list, &count) == KERN_SUCCESS);
    //     for(int i=0; i<count; i++) {
    //         if(list[i] != mach_thread_self()) { //mach_port_deallocate
    //             assert(thread_suspend(list[i]) == KERN_SUCCESS);
    //         }
    //     }
    // }

if(!textaddr) {
    struct mach_header_64* header = _dyld_get_prog_image_header(); //_NSGetMachExecuteHeader()
    struct load_command* lc = (struct load_command*)((uint64_t)header + sizeof(*header));
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64)
        {
            struct segment_command_64 * seg = (struct segment_command_64 *) lc;
                forklog("%s-%d segment: %s file=%llx:%llx vm=%p:%llx\n", tag, flag, seg->segname, seg->fileoff, seg->filesize, (void*)seg->vmaddr, seg->vmsize);

            if(strcmp(seg->segname, SEG_PAGEZERO) != 0) {

				vmSpaceSize += seg->vmsize;
			}


            if(strcmp(seg->segname, SEG_TEXT)==0)
            {
                forklog("%s-%d segment: %s file=%llx:%llx vm=%p:%llx\n", tag, flag, seg->segname, seg->fileoff, seg->filesize, (void*)seg->vmaddr, seg->vmsize);

                //According to dyld, the __TEXT address is always equal to the header address

				textaddr = (uint64_t)header;
				textsize = seg->vmsize;

                //break;
            }
        }
        lc = (struct load_command *) ((char *)lc + lc->cmdsize);
    }

	forklog("executable header=%p, textsize=%lx vmSpaceSize=%lx", header, textsize, vmSpaceSize);
}

				// 	char exepath[PATH_MAX]={0};
				// 	uint32_t pathsize=sizeof(exepath);
				// 	_NSGetExecutablePath(exepath, &pathsize);
				// 	forklog("exepath=%s", exepath);


				// {
				// 	// int ret = munmap((void*)textaddr, textsize);
				// 	// forklog("munmap=%d,%s", ret, strerror(errno));

				// 	// kr = vm_deallocate(task, (vm_address_t)textaddr, textsize);
				// 	// forklog("vm_deallocate=%d %s", kr, mach_error_string(kr));


				// 	// showvm(task, (uint64_t)textaddr, textsize);



				// 	vm_address_t loadAddress = (vm_address_t)textaddr;
				// 	kr = vm_allocate(task, &loadAddress, (vm_size_t)textsize, VM_FLAGS_ANYWHERE);
				// 	forklog("vm_allocate=%llx, %x %s", loadAddress, kr, mach_error_string(kr));


				// 	int fd = open(exepath, O_RDONLY);
				// 	forklog("fd=%d",fd);
				// 	void* mapaddr = mmap((void*)loadAddress, (size_t)textsize, PROT_READ|PROT_EXEC, MAP_FIXED|MAP_PRIVATE, fd, 0);
				// 	forklog("mmap=%p,%s", mapaddr, strerror(errno));

				// 	int prot=0, maxprot=0;
				// 	vm_address_t remapaddr = textaddr;
				// 	kr = vm_remap(task, &remapaddr, textsize, 0, VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE, task, loadAddress, false, &prot, &maxprot, VM_INHERIT_SHARE);
				// 	forklog("vm_remap=%llx,%d/%d : %x %s", remapaddr, prot, maxprot, kr, mach_error_string(kr));

				// 	close(fd);
				// }



				// kr = _mach_vm_protect(task, (vm_address_t)textaddr, textsize, false, flag ? VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY : VM_PROT_READ|VM_PROT_EXECUTE);
				// if(kr != KERN_SUCCESS) {
				// 	forklog("[%d] %s[%d] vm_protect failed! %d,%s\n", getpid(), tag, flag,  kr, mach_error_string(kr));
				// 	//abort();
				// } else {
				// 	forklog("[%d] %s[%d] vm_protect success @ %p,%llx\n", getpid(), tag, flag,  (void*)textaddr, textsize);
				// }


				if(flag) {
					// kr = _mach_vm_protect(task, (vm_address_t)textaddr, textsize, false, VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
					// forklog("[%d] %s[%d] vm_protect %d,%s\n", getpid(), tag, flag,  kr, mach_error_string(kr));
					// *(int*)(textaddr+0x1C) = 1;
					kr = _mach_vm_protect(task, (vm_address_t)textaddr, textsize, false, VM_PROT_READ);
					forklog("[%d] %s[%d] vm_protect %d,%s\n", getpid(), tag, flag,  kr, mach_error_string(kr));
					
				} else {
					// kr = _mach_vm_protect(task, (vm_address_t)textaddr, textsize, false, VM_PROT_READ|VM_PROT_WRITE);
					// forklog("[%d] %s[%d] vm_protect %d,%s\n", getpid(), tag, flag,  kr, mach_error_string(kr));
					// *(int*)(textaddr+0x1C) = 2;
					kr = _mach_vm_protect(task, (vm_address_t)textaddr, textsize, false, VM_PROT_READ|VM_PROT_EXECUTE);
					forklog("[%d] %s[%d] vm_protect %d,%s\n", getpid(), tag, flag,  kr, mach_error_string(kr));
				}


				// assert(*(int*)textaddr);

                //showvm(task_self_trap(), (uint64_t)textaddr, textsize); 

    // if(!flag) {
    //     int count=0;
    //     thread_act_array_t list;
    //     assert(task_threads(mach_task_self(), &list, &count) == KERN_SUCCESS);
    //     for(int i=0; i<count; i++) {
    //         if(list[i] != mach_thread_self()) { //mach_port_deallocate
    //             assert(thread_resume(list[i]) == KERN_SUCCESS);
    //         }
    //     }
    // }   
}



extern pid_t __fork(void);
extern pid_t __vfork(void);

static void (**_libSystem_atfork_prepare)(void) = 0;
static void (**_libSystem_atfork_parent)(void) = 0;
static void (**_libSystem_atfork_child)(void) = 0;
static void (**_libSystem_atfork_prepare_v2)(unsigned int flags, ...) = 0;
static void (**_libSystem_atfork_parent_v2)(unsigned int flags, ...) = 0;
static void (**_libSystem_atfork_child_v2)(unsigned int flags, ...) = 0;

#define RESOVLE_ATFORK(n)  {\
*(void**)&n = DobbySymbolResolver("/usr/lib/system/libsystem_c.dylib", #n);\
  SYSLOG("forkfunc %s = %p:%p", #n, n, n?*n:NULL);\
  }

#include "dobby.h"
static void 
//__attribute__((__constructor__)) 
atforkinit()
{
    RESOVLE_ATFORK(_libSystem_atfork_prepare);
    RESOVLE_ATFORK(_libSystem_atfork_parent);
    RESOVLE_ATFORK(_libSystem_atfork_child);
    RESOVLE_ATFORK(_libSystem_atfork_prepare_v2);
    RESOVLE_ATFORK(_libSystem_atfork_parent_v2);
    RESOVLE_ATFORK(_libSystem_atfork_child_v2);
}

//redefine for pointers
#define _libSystem_atfork_prepare		(*_libSystem_atfork_prepare)
#define _libSystem_atfork_parent  		(*_libSystem_atfork_parent)
#define _libSystem_atfork_child  		(*_libSystem_atfork_child)
#define _libSystem_atfork_prepare_v2  	(*_libSystem_atfork_prepare_v2)
#define _libSystem_atfork_parent_v2  	(*_libSystem_atfork_parent_v2)
#define _libSystem_atfork_child_v2  	(*_libSystem_atfork_child_v2)

// struct sigaction fork_oldact = {0};
// void forksig(int signo, siginfo_t *info, void *context)
// {
//     forklog("%d forksig %d %p\n", getpid(), info->si_pid, fork_oldact.sa_sigaction);

//     forkfix("sig", false);

//     if(fork_oldact.sa_sigaction) {
//         fork_oldact.sa_sigaction(signo, info, context);
//     }
// }



extern void _malloc_fork_prepare(void);
extern void _malloc_fork_parent(void);
extern void xpc_atfork_prepare(void);
extern void xpc_atfork_parent(void);
extern void dispatch_atfork_prepare(void);
extern void dispatch_atfork_parent(void);

int childToParentPipe[2];
int parentToChildPipe[2];
static void openPipes(void)
{
	if (pipe(parentToChildPipe) < 0 || pipe(childToParentPipe) < 0) {
		abort();
	}
}
static void closePipes(void)
{
	if (close(parentToChildPipe[0]) != 0 || close(parentToChildPipe[1]) != 0 || close(childToParentPipe[0]) != 0 || close(childToParentPipe[1]) != 0) {
		abort();
	}
}

void child_fixup(void)
{
	// Tell parent we are waiting for fixup now
	char msg = ' ';
	write(childToParentPipe[1], &msg, sizeof(msg));

	// Wait until parent completes fixup
	while((read(parentToChildPipe[0], &msg, sizeof(msg))<=0) && errno==EINTR){}; //may be interrupted by ptrace

}

void parent_fixup(pid_t childPid)
{
	// Reenable some system functionality that XPC is dependent on and XPC itself
	// (Normally unavailable during __fork)
	_malloc_fork_parent();
	dispatch_atfork_parent();
	xpc_atfork_parent();

	// Wait until the child is ready and waiting
	char msg = ' ';
	read(childToParentPipe[0], &msg, sizeof(msg));

	// // Child is waiting for wx_allowed + permission fixups now
	// // Apply fixup
	// int64_t fix_ret = jbdswForkFix(childPid);
	// if (fix_ret != 0) {
	// 	kill(childPid, SIGKILL);
	// 	abort();
	// }
	
	int bsd_enableJIT2(pid_t pid);
	assert(bsd_enableJIT2(childPid)==0);

	// Tell child we are done, this will make it resume
	write(parentToChildPipe[1], &msg, sizeof(msg));

	// Disable system functionality related to XPC again
	_malloc_fork_prepare();
	dispatch_atfork_prepare();
	xpc_atfork_prepare();
}

pid_t __fork1(void)
{
	openPipes();

	pid_t pid = __fork();
	if (pid < 0) {
		closePipes();
		return pid;
	}

	if (pid == 0) {
		child_fixup();
	}
	else {
		parent_fixup(pid);
	}

	closePipes();
	return pid;
}

#define LIBSYSTEM_ATFORK_HANDLERS_ONLY_FLAG 1

static inline __attribute__((always_inline))
pid_t
_do_fork(bool libsystem_atfork_handlers_only)
{
	// assert(jbdswDebugMe()==0);

	static int atforkinited=0;
	if(atforkinited++==0) atforkinit();

	forklog("atfork inited");

	int ret;

	int flags = libsystem_atfork_handlers_only ? LIBSYSTEM_ATFORK_HANDLERS_ONLY_FLAG : 0;

	if (_libSystem_atfork_prepare_v2) {
		_libSystem_atfork_prepare_v2(flags);
	} else {
		_libSystem_atfork_prepare();
	}
	// Reader beware: this __fork() call is yet another wrapper around the actual syscall
	// and lives inside libsyscall. The fork syscall needs some cuddling by asm before it's
	// allowed to see the big wide C world.


	// struct sigaction act = {0};
    // struct sigaction oldact = {0};

	// act.sa_flags = SA_ONSTACK|SA_NODEFER|SA_SIGINFO;
    // act.sa_sigaction = forksig;
	// sigfillset(&act.sa_mask);

	// sigaction(SIGCHLD, &act, &oldact);
    // if(oldact.sa_sigaction != forksig) fork_oldact = oldact;
    // forklog("oldact=%x %x %p\n", oldact.sa_flags, oldact.sa_mask, oldact.sa_sigaction);

	// for(int i=0; i<999; i++) sigignore(i);

    forklog("do fork %d\n", getpid());

    sigset_t newmask, oldmask;
    sigfillset(&newmask);
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    forklog("fork fix %d\n", getpid());
    forkfix(libsystem_atfork_handlers_only?"vfork":"fork", true, false);
	
	pid_t pid = ret = __fork1();
    forklog("forked %d\n", pid);

    forkfix(libsystem_atfork_handlers_only?"vfork":"fork", false, pid==0);
    forklog("fork fixed %d\n", getpid());

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

	if (-1 == ret)
	{
		// __fork already set errno for us
		if (_libSystem_atfork_parent_v2) {
			_libSystem_atfork_parent_v2(flags);
		} else {
			_libSystem_atfork_parent();
		}
		return ret;
	}

	if (0 == ret)
	{
		// We're the child in this part.
		if (_libSystem_atfork_child_v2) {
			_libSystem_atfork_child_v2(flags);
		} else {
			_libSystem_atfork_child();
		}
		return 0;
	}

	if (_libSystem_atfork_parent_v2) {
		_libSystem_atfork_parent_v2(flags);
	} else {
		_libSystem_atfork_parent();
	}
	return ret;
}

pid_t
_fork(void)
{
	return _do_fork(false);
}

pid_t
_vfork(void)
{
	// vfork() is now just fork().
	// Skip the API pthread_atfork handlers, but do call our own
	// Libsystem_atfork handlers. People are abusing vfork in ways where
	// it matters, e.g. tcsh does all kinds of stuff after the vfork. Sigh.
	return _do_fork(true);
}




int
_forkpty(int *aprimary, char *name, struct termios *termp, struct winsize *winp)
{
	int primary, replica, pid;

	if (openpty(&primary, &replica, name, termp, winp) == -1)
		return (-1);
	switch (pid = fork()) {
	case -1:
		return (-1);
	case 0:
		/* 
		 * child
		 */
		(void) close(primary);
		/*
		 * 4300297: login_tty() may fail to set the controlling tty.
		 * Since we have already forked, the best we can do is to 
		 * dup the replica as if login_tty() succeeded.
		 */
		if (login_tty(replica) < 0) {
			syslog(LOG_ERR, "forkpty: login_tty could't make controlling tty");
			(void) dup2(replica, 0);
			(void) dup2(replica, 1);
			(void) dup2(replica, 2);
			if (replica > 2)
				(void) close(replica);
		}
		return (0);
	}
	/*
	 * parent
	 */
	*aprimary = primary;
	(void) close(replica);
	return (pid);
}


#define _dup2 dup2
#define _open open
#define _close close
#define _sigaction sigaction
#include <paths.h>
#include <fcntl.h>
#ifndef VARIANT_PRE1050
#include <mach/mach_init.h>
#include <bootstrap.h>
static void
move_to_root_bootstrap(void)
{
	mach_port_t parent_port = 0;
	mach_port_t previous_port = 0;

	do {
		if (previous_port) {
			mach_port_deallocate(mach_task_self(), previous_port);
			previous_port = parent_port;
		} else {
			previous_port = bootstrap_port;
		}

		if (bootstrap_parent(previous_port, &parent_port) != 0) {
			return;
		}
	} while (parent_port != previous_port);

	task_set_bootstrap_port(mach_task_self(), parent_port);
	bootstrap_port = parent_port;
}
#endif /* !VARIANT_PRE1050 */

int daemon(int, int) __DARWIN_1050(daemon);

int
_daemon(nochdir, noclose)
	int nochdir, noclose;
{
	struct sigaction osa, sa;
	int fd;
	pid_t newgrp;
	int oerrno;
	int osa_ok;

	/* A SIGHUP may be thrown when the parent exits below. */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	osa_ok = _sigaction(SIGHUP, &sa, &osa);
#ifndef VARIANT_PRE1050
	move_to_root_bootstrap();
#endif /* !VARIANT_PRE1050 */
	switch (fork()) {
	case -1:
		return (-1);
	case 0:
		break;
	default:
		_exit(0);
	}

	newgrp = setsid();
	oerrno = errno;
	if (osa_ok != -1)
		_sigaction(SIGHUP, &osa, NULL);

	if (newgrp == -1) {
		errno = oerrno;
		return (-1);
	}

	if (!nochdir)
		(void)chdir("/");

	if (!noclose && (fd = _open(_PATH_DEVNULL, O_RDWR, 0)) != -1) {
		(void)_dup2(fd, STDIN_FILENO);
		(void)_dup2(fd, STDOUT_FILENO);
		(void)_dup2(fd, STDERR_FILENO);
		if (fd > 2)
			(void)_close(fd);
	}
	return (0);
}
