#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "list.h"
#include "process.h"
/***************************************************************************************************************************************************************/

//Function Prototyping

static void syscall_handler (struct intr_frame *);
void* check_addr(const void*);
struct proc_file* list_search(struct list* files, int fd);

extern bool running;

struct proc_file {
	struct file* ptr;
	int fd;
	struct list_elem elem;
};

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}
/***************************************************************************************************************************************************************
    
PSUEDO CODE for syscall_handler:

Step 1: First ensure that the system call argument is a valid address. If not, exit immediately.

Step 2: Get the value of the system call (based on enum in syscall-nr.h) and call corresponding syscall function. 

Step 3: Handle WAIT Syscall
        -> Fetch arguments from stack : Only one ARG -> esp + 5 (PID of the child process that the current process must wait on.)
        -> Call process_wait() function

Step 4: Handle EXIT Syscall
        -> Fetch arguments from stack : Only one ARG -> esp + 5 (stores exit status)
        -> Set status of current thread = exit status
	-> Print the status
	-> Terminate the thread using thread_exit()

Step 5: Handle EXEC Syscall
        -> Fetch arguments from stack : Only one ARG -> esp + 5 (entire command line text for executing the program)
        -> Return if cmdline is NULL
        -> Otherwise, lock file_sys to avoid race condition
        -> Call process_execute(cmdline), (present in process.c) that parses cmdline and runs the cmd
	-> Release lock & return the PID of the thread that is created 
	-> Store the returned value in eax register

Step 6: Handle WRITE Syscall
        -> Fetch arguments from stack : 3 ARGs 
	-> ARG1 = esp + 5 ( File Descriptor )
	-> ARG2 = esp + 6 ( Pointer to Buffer )
	-> ARG3 = esp + 7 ( Buffer Size )
	-> Lock file_sys to avoid race condition
	-> if FD=1 (STDOUT), then write to console using putbuf(buffer, size);
	-> Release lock & return number of bytes written 
	-> Otherwise, release the lock & retuen -1
	-> Store the returned value in eax register

Step 7: Terminate the program in case of an invalid syscall number
/***************************************************************************************************************************************************************/

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int * p = f->esp;

	check_addr(p);



  int system_call = * p;
	switch (system_call)
	{
		case SYS_EXIT:
		check_addr(p+1);     // Exit has exactly one stack argument, representing the exit status.
		exit(*(p+1));        // We pass exit the status code of the process.
		break;

		case SYS_EXEC:
		check_addr(p+1);      // The first argument of exec is the entire command line text for executing the program 
		check_addr(*(p+1));
		f->eax = exec_proc(*(p+1)); //Store the Return value of the exec_proc() function in the eax register.
		break;

		case SYS_WAIT:
		check_addr(p+1);             // The first argument is the PID of the child process that the current process must wait on.
		f->eax = process_wait(*(p+1));  // Return the result of the process_wait() function in the eax register.
		break;

		case SYS_WRITE:
		//Three arguments off of the stack : The first represents the fd, the second represents the buffer, and the third represents the buffer size.
		check_addr(p+7);          //Check Buffer Size
		check_addr(*(p+6));       //Check Buffer Poiniter
		if(*(p+5)==1)
		{
			putbuf(*(p+6),*(p+7)); // If fd is equal to one, then we write to STDOUT using putbuf function.
			f->eax = *(p+7);
		}
		else
		{
			struct proc_file* fptr = list_search(&thread_current()->files, *(p+5));
			if(fptr==NULL)
				f->eax=-1;
			else
			{
				acquire_filesys_lock();
				f->eax = file_write (fptr->ptr, *(p+6), *(p+7));
				release_filesys_lock();
			}
		}
		break;



		default:
		printf("Default %d\n",*p);
	}
}

/***************************************************************************************************************************************************************/

int exec_proc(char *file_name)
{
	acquire_filesys_lock();
	char * fn_cp = malloc (strlen(file_name)+1);
	  strlcpy(fn_cp, file_name, strlen(file_name)+1);
	  
	  char * save_ptr;
	  fn_cp = strtok_r(fn_cp," ",&save_ptr);

	 struct file* f = filesys_open (fn_cp);

	  if(f==NULL)
	  {
	  	release_filesys_lock();
	  	return -1;
	  }
	  else
	  {
	  	file_close(f);
	  	release_filesys_lock();
	  	return process_execute(file_name);
	  }
}

/***************************************************************************************************************************************************************/

void exit(int status)
{
	//printf("Exit : %s %d %d\n",thread_current()->name, thread_current()->tid, status);
	struct list_elem *e;

      for (e = list_begin (&thread_current()->parent->child_proc); e != list_end (&thread_current()->parent->child_proc);
           e = list_next (e))
        {
          struct child *f = list_entry (e, struct child, elem);
          if(f->tid == thread_current()->tid)
          {
          	f->used = true;
          	f->exit_error = status;
          }
        }


	thread_current()->exit_error = status;

	if(thread_current()->parent->waiting_on == thread_current()->tid)
		sema_up(&thread_current()->parent->child_lock);

	thread_exit();
}

/***************************************************************************************************************************************************************/

void* check_addr(const void *vaddr)
{
	//Function: bool is_user_vaddr (const void *va) 
	//Returns true if va is a user virtual address, false otherwise. 
  
	if (!is_user_vaddr(vaddr))
	{
		exit(-1);
		return 0;
	}
	//Function: void *pagedir_get_page (uint32_t *pd, const void *uaddr)
    	//Looks up the frame mapped to uaddr in pd. Returns the kernel virtual address for that frame, if uaddr is mapped, or a null pointer if it is not. 
        
	void *ptr = pagedir_get_page(thread_current()->pagedir, vaddr);
	if (!ptr)
	{
		exit(-1);
		return 0;
	}
	return ptr;
}

/***************************************************************************************************************************************************************/

struct proc_file* list_search(struct list* files, int fd)
{

	struct list_elem *e;

      for (e = list_begin (files); e != list_end (files);
           e = list_next (e))
        {
          struct proc_file *f = list_entry (e, struct proc_file, elem);
          if(f->fd == fd)
          	return f;
        }
   return NULL;
}

void close_file(struct list* files, int fd)
{

	struct list_elem *e;

	struct proc_file *f;

      for (e = list_begin (files); e != list_end (files);
           e = list_next (e))
        {
          f = list_entry (e, struct proc_file, elem);
          if(f->fd == fd)
          {
          	file_close(f->ptr);
          	list_remove(e);
          }
        }

    free(f);
}

void close_all_files(struct list* files)
{

	struct list_elem *e;

	while(!list_empty(files))
	{
		e = list_pop_front(files);

		struct proc_file *f = list_entry (e, struct proc_file, elem);
          
	      	file_close(f->ptr);
	      	list_remove(e);
	      	free(f);


	}

      
}