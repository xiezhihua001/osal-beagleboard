/*
** File   : osapi.c
**
**      Copyright (c) 2004-2006, United States government as represented by the 
**      administrator of the National Aeronautics Space Administration.  
**      All rights reserved. This software was created at NASAs Goddard 
**      Space Flight Center pursuant to government contracts.
**
**      This is governed by the NASA Open Source Agreement and may be used, 
**      distributed and modified only pursuant to the terms of that agreement.
**
** Author : Alan Cudmore
**
** Purpose: 
**         This file  contains some of the OS APIs abstraction layer 
**         implementation for POSIX, specifically for Linux and Mac OS X.
**      
**         This is a consolidated release for POSIX OSs. The ifdefs currently used 
**          are: _MAC_OS, and _LINUX_OS_
**
** $Date: 2012/01/09 17:28:43EST $
** $Revision: 1.12 $
** $Log: osapi.c  $
** Revision 1.12 2012/01/09 17:28:43EST acudmore 
** Fixed a couple of OS X related compilation errors
** Revision 1.11 2011/12/05 15:26:29EST acudmore 
** Added semaphore protection for counting semaphore give and take operations
** Revision 1.10 2011/06/27 15:50:18EDT acudmore 
** Went over APIs and Documentation for return code consistency.
** Updated documentation, function comments, and return codes as needed.
** Revision 1.9 2011/04/05 12:53:49EDT acudmore 
** Updated comments for timeout code. If a pend is interrupted by 
** a system call, there is no need to recompute delay
** Revision 1.8 2011/03/30 11:43:47EDT acudmore 
** Updated all timeout code to:
** - use the CompAbsDelayTime function
** - Check to see if the delay was interrupted by a system call
** - Not use a busy loop if the posix call is available ( linux )
** Revision 1.7 2011/03/23 12:31:52EDT acudmore 
** Updated pending calls to account for EINTR return code ( interrupted by signal )
** Revision 1.6 2011/03/23 11:20:09EDT acudmore 
** Added logic to posix message queue create and delete to make queue name unique for each process running the cfe
** Revision 1.5 2010/11/12 12:00:43EST acudmore 
** replaced copyright character with (c) and added open source notice where needed.
** Revision 1.4 2010/11/10 15:33:47EST acudmore 
** Fixed IntAttachHandler prototype
** Revision 1.3 2010/03/10 15:43:03EST acudmore 
** Updated to work with cygwin 1.7.x
** Revision 1.2 2010/03/08 12:07:17EST acudmore 
** fixed warnings by using a function pointer type
** Revision 1.1 2010/02/17 13:12:01EST acudmore 
** Initial revision
** Member added to project c:/MKSDATA/MKS-REPOSITORY/MKS-OSAL-REPOSITORY/src/os/posix/project.pj
** Member added to project c:/MKSDATA/MKS-REPOSITORY/MKS-OSAL-REPOSITORY/src/os/linux/project.pj
** Revision 1.10 2008/02/14 10:12:24EST apcudmore 
** initialized pthread_attr_t to fix problem with OS_TaskCreate in cygwin
** Revision 1.9 2008/02/04 10:59:00EST apcudmore 
** Changed OS_QueueGet timeout to use select rather than a sleep loop.
** Revision 1.7 2008/01/31 10:37:05EST apcudmore 
** Implement Task Delete hook API.
*/

/****************************************************************************************
                                    INCLUDE FILES
****************************************************************************************/
#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>
#include <signal.h>

#include <errno.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>     
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#include <limits.h>

/*
** User defined include files
*/
#include "common_types.h"
#include "osapi.h"

/*
** This include must be put below the osapi.h
** include so it can pick up the define
*/
#ifndef OSAL_SOCKET_QUEUE
#include <mqueue.h>
#endif

/*
** Defines
*/
#define OS_BASE_PORT 43000
#define UNINITIALIZED 0
#define MAX_PRIORITY 255
#ifndef PTHREAD_STACK_MIN
   #define PTHREAD_STACK_MIN 8092
#endif

/*
** Global data for the API
*/

/*  
** Tables for the properties of objects 
*/

/*tasks */
typedef struct
{
    int       free;
    pthread_t id;
    char      name [OS_MAX_API_NAME];
    int       creator;
    uint32    stack_size;
    uint32    priority;
    void     *delete_hook_pointer;
}OS_task_record_t;
    
#ifdef OSAL_SOCKET_QUEUE
/* queues */
typedef struct
{
    int free;
    int id;
    char name [OS_MAX_API_NAME];
    int creator;
}OS_queue_record_t;
#else
/* queues */
typedef struct
{
    int   free;
    mqd_t id;
    char  name [OS_MAX_API_NAME];
    int   creator;
}OS_queue_record_t;
#endif

/* Binary Semaphores */
typedef struct
{
    int free;
#ifdef _MAC_OS_
    sem_t *id;
#else
    sem_t id;
#endif
    char name [OS_MAX_API_NAME];
    int creator;
    int max_value;
    int current_value;
}OS_bin_sem_record_t;

/*Counting Semaphores */
typedef struct
{
    int free;
#ifdef _MAC_OS_
    sem_t *id;
#else
    sem_t id;
#endif
    char name [OS_MAX_API_NAME];
    int creator;
    int max_value;
    int current_value;
}OS_count_sem_record_t;

/* Mutexes */
typedef struct
{
    int free;
    pthread_mutex_t id;
    char name [OS_MAX_API_NAME];
    int creator;
    int nested_value; 
}OS_mut_sem_record_t;

/* function pointer type */
typedef void (*FuncPtr_t)(void);

/* Tables where the OS object information is stored */
OS_task_record_t    OS_task_table          [OS_MAX_TASKS];
OS_queue_record_t   OS_queue_table         [OS_MAX_QUEUES];
OS_bin_sem_record_t OS_bin_sem_table       [OS_MAX_BIN_SEMAPHORES];
OS_count_sem_record_t OS_count_sem_table   [OS_MAX_COUNT_SEMAPHORES];
OS_mut_sem_record_t OS_mut_sem_table       [OS_MAX_MUTEXES];

pthread_key_t    thread_key;

pthread_mutex_t OS_task_table_mut;
pthread_mutex_t OS_queue_table_mut;
pthread_mutex_t OS_bin_sem_table_mut;
pthread_mutex_t OS_mut_sem_table_mut;
pthread_mutex_t OS_count_sem_table_mut;
/*
** Local Function Prototypes
*/
uint32  OS_CompAbsDelayTime( uint32 milli_second , struct timespec * tm);
void    OS_ThreadKillHandler(int sig );
uint32  OS_FindCreator(void);
int32   OS_PriorityRemap(uint32 InputPri);

/*---------------------------------------------------------------------------------------
   Name: OS_API_Init

   Purpose: Initialize the tables that the OS API uses to keep track of information
            about objects

   returns: OS_SUCCESS or OS_ERROR
---------------------------------------------------------------------------------------*/
int32 OS_API_Init(void)
{
   int    i;
   int    ret;
   int32 return_code = OS_SUCCESS;
    
    /* Initialize Task Table */
   
   for(i = 0; i < OS_MAX_TASKS; i++)
   {
        OS_task_table[i].free                = TRUE;
        OS_task_table[i].creator             = UNINITIALIZED;
        OS_task_table[i].delete_hook_pointer = NULL;
        strcpy(OS_task_table[i].name,"");    
    }

    /* Initialize Message Queue Table */

    for(i = 0; i < OS_MAX_QUEUES; i++)
    {
        OS_queue_table[i].free        = TRUE;
        OS_queue_table[i].id          = UNINITIALIZED;
        OS_queue_table[i].creator     = UNINITIALIZED;
        strcpy(OS_queue_table[i].name,""); 
    }

    /* Initialize Binary Semaphore Table */

    for(i = 0; i < OS_MAX_BIN_SEMAPHORES; i++)
    {
        OS_bin_sem_table[i].free        = TRUE;
        OS_bin_sem_table[i].creator     = UNINITIALIZED;
        strcpy(OS_bin_sem_table[i].name,"");
    }

    /* Initialize Counting Semaphores */
    for(i = 0; i < OS_MAX_BIN_SEMAPHORES; i++)
    {
        OS_count_sem_table[i].free        = TRUE;
        OS_count_sem_table[i].creator     = UNINITIALIZED;
        strcpy(OS_count_sem_table[i].name,"");
    }
    /* Initialize Mutex Semaphore Table */

    for(i = 0; i < OS_MAX_MUTEXES; i++)
    {
        OS_mut_sem_table[i].free        = TRUE;
        OS_mut_sem_table[i].creator     = UNINITIALIZED;
        OS_mut_sem_table[i].nested_value = 0;
        strcpy(OS_mut_sem_table[i].name,"");
    }

   /*
   ** Initialize the module loader
   */
   #ifdef OS_INCLUDE_MODULE_LOADER
      return_code = OS_ModuleTableInit();
      if ( return_code == OS_ERROR )
      {
         return(return_code);
      }
   #endif

   /*
   ** Initialize the Timer API
   */
   return_code = OS_TimerAPIInit();
   if ( return_code == OS_ERROR )
   {
      return(return_code);
   }

   ret = pthread_key_create(&thread_key, NULL );
   if ( ret != 0 )
   {
      printf("Error creating thread key\n");
      return_code = OS_ERROR;
      return(return_code);
   }

   /*
   ** create the mutexes that protect the OSAPI structures 
   ** the function returns on error, since we dont want to go through
   ** the extra trouble of creating and deleting resoruces for nothing
   */   
   ret = pthread_mutex_init((pthread_mutex_t *) & OS_task_table_mut,NULL);
   if ( ret != 0 )
   {
      return_code = OS_ERROR;
      return(return_code);
   }
   ret = pthread_mutex_init((pthread_mutex_t *) & OS_queue_table_mut,NULL); 
   if ( ret != 0 )
   {
      return_code = OS_ERROR;
      return(return_code);
   }
   ret = pthread_mutex_init((pthread_mutex_t *) & OS_bin_sem_table_mut,NULL); 
   if ( ret != 0 )
   {
      return_code = OS_ERROR;
      return(return_code);
   }
   ret = pthread_mutex_init((pthread_mutex_t *) & OS_count_sem_table_mut,NULL); 
   if ( ret != 0 )
   {
      return_code = OS_ERROR;
      return(return_code);
   }
   ret = pthread_mutex_init((pthread_mutex_t *) & OS_mut_sem_table_mut,NULL); 
   if ( ret != 0 )
   {
      return_code = OS_ERROR;
      return(return_code);
   }

   /*
   ** File system init
   */
   return_code = OS_FS_Init();

   return(return_code);
   
}

/*
**********************************************************************************
**          TASK API
**********************************************************************************
*/

/*---------------------------------------------------------------------------------------
   Name: OS_TaskCreate

   Purpose: Creates a task and starts running it.

   returns: OS_INVALID_POINTER if any of the necessary pointers are NULL
            OS_ERR_NAME_TOO_LONG if the name of the task is too long to be copied
            OS_ERR_INVALID_PRIORITY if the priority is bad
            OS_ERR_NO_FREE_IDS if there can be no more tasks created
            OS_ERR_NAME_TAKEN if the name specified is already used by a task
            OS_ERROR if the operating system calls fail
            OS_SUCCESS if success
            
    NOTES: task_id is passed back to the user as the ID. stack_pointer is usually null.
           Flags are unused at this point.

---------------------------------------------------------------------------------------*/
int32 OS_TaskCreate (uint32 *task_id, const char *task_name, osal_task_entry function_pointer,
                      const uint32 *stack_pointer, uint32 stack_size, uint32 priority,
                      uint32 flags)
{
    int                return_code = 0;
    pthread_attr_t     custom_attr ;
    struct sched_param priority_holder ;
    int                possible_taskid;
    int                i;
    uint32             local_stack_size;
    int                ret;  
    int                os_priority;

    
    /* Check for NULL pointers */    
    if( (task_name == NULL) || (function_pointer == NULL) || (task_id == NULL) )
    {
        return OS_INVALID_POINTER;
    }
    
    /* we don't want to allow names too long*/
    /* if truncated, two names might be the same */
    if (strlen(task_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    /* Check for bad priority */
    if (priority > MAX_PRIORITY)
    {
        return OS_ERR_INVALID_PRIORITY;
    }

    /* Change OSAL priority into a priority that will work for this OS */
    os_priority = OS_PriorityRemap(priority);
    
    /* Check Parameters */
    pthread_mutex_lock(&OS_task_table_mut); 

    for(possible_taskid = 0; possible_taskid < OS_MAX_TASKS; possible_taskid++)
    {
        if (OS_task_table[possible_taskid].free == TRUE)
        {
            break;
        }
    }

    /* Check to see if the id is out of bounds */
    if( possible_taskid >= OS_MAX_TASKS || OS_task_table[possible_taskid].free != TRUE)
    {
        pthread_mutex_unlock(&OS_task_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }

    /* Check to see if the name is already taken */ 
    for (i = 0; i < OS_MAX_TASKS; i++)
    {
        if ((OS_task_table[i].free == FALSE) &&
           ( strcmp((char*) task_name, OS_task_table[i].name) == 0)) 
        {       
            pthread_mutex_unlock(&OS_task_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    }
    
    /* 
    ** Set the possible task Id to not free so that
    ** no other task can try to use it 
    */
    OS_task_table[possible_taskid].free = FALSE;
    
    pthread_mutex_unlock(&OS_task_table_mut);

    if ( stack_size < PTHREAD_STACK_MIN )
    {
       local_stack_size = PTHREAD_STACK_MIN;
    }
    else
    {
        local_stack_size = stack_size;
    }

   /*
   ** Initialize the pthread_attr structure. 
   ** The structure is used to set the stack and priority
   */
    memset(&custom_attr, 0, sizeof(custom_attr));
    if(pthread_attr_init(&custom_attr))
    {  
        pthread_mutex_lock(&OS_task_table_mut); 
        OS_task_table[possible_taskid].free = TRUE;
        pthread_mutex_unlock(&OS_task_table_mut); 
        printf("pthread_attr_init error in OS_TaskCreate, Task ID = %d\n",possible_taskid);
		  perror("pthread_attr_init");
        /* return(OS_ERROR); */
    }

    /*
    ** Set the Stack Size
    */
    if (pthread_attr_setstacksize(&custom_attr, (size_t)local_stack_size ))
    {
        printf("pthread_attr_setstacksize error in OS_TaskCreate, Task ID = %d\n",possible_taskid);
        /* return(OS_ERROR); Disabled for older versions of linux */
    }
        
    /* 
    ** Set priority 
    */
    priority_holder.sched_priority = os_priority;
    ret = pthread_attr_setschedparam(&custom_attr,&priority_holder);
    if(ret !=0)
    {
       /*return(OS_ERROR); Disabled for older versions of linux */
    }

    /*
    ** Create thread
    */
    return_code = pthread_create(&(OS_task_table[possible_taskid].id),
                                 &custom_attr,
                                 function_pointer,
                                 0);
    if (return_code != 0)
    {
        pthread_mutex_lock(&OS_task_table_mut); 
        OS_task_table[possible_taskid].free = TRUE;
        pthread_mutex_unlock(&OS_task_table_mut); 
        printf("pthread_create error in OS_TaskCreate, Task ID = %d\n",possible_taskid);
        return(OS_ERROR);
    }

    /*
    ** Free the resources that are no longer needed
    */
    return_code = pthread_detach(OS_task_table[possible_taskid].id);
    if (return_code !=0)
    {
       pthread_mutex_lock(&OS_task_table_mut);
       OS_task_table[possible_taskid].free = TRUE;
       pthread_mutex_unlock(&OS_task_table_mut);
       printf("pthread_detach error in OS_TaskCreate, Task ID = %d\n",possible_taskid);
       return(OS_ERROR);
    }

    return_code = pthread_attr_destroy(&custom_attr);
    if (return_code !=0)
    {
       pthread_mutex_lock(&OS_task_table_mut);
       OS_task_table[possible_taskid].free = TRUE;
       pthread_mutex_unlock(&OS_task_table_mut);
       printf("pthread_attr_destroy error in OS_TaskCreate, Task ID = %d\n",possible_taskid);
       return(OS_ERROR);
    }

    /*
    ** Assign the task ID
    */
    *task_id = possible_taskid;

    /* 
    ** This Id no longer free 
    */
    OS_task_table[possible_taskid].free = FALSE;

    pthread_mutex_lock(&OS_task_table_mut); 

    strcpy(OS_task_table[*task_id].name, (char*) task_name);

    OS_task_table[possible_taskid].creator = OS_FindCreator();
    OS_task_table[possible_taskid].stack_size = stack_size;
    /* Use the abstracted priority, not the OS one */
    OS_task_table[possible_taskid].priority = priority;

    pthread_mutex_unlock(&OS_task_table_mut);

    return OS_SUCCESS;
}/* end OS_TaskCreate */


/*--------------------------------------------------------------------------------------
     Name: OS_TaskDelete

    Purpose: Deletes the specified Task and removes it from the OS_task_table.

    returns: OS_ERR_INVALID_ID if the ID given to it is invalid
             OS_ERROR if the OS delete call fails
             OS_SUCCESS if success
---------------------------------------------------------------------------------------*/
int32 OS_TaskDelete (uint32 task_id)
{    
    int       ret;
    FuncPtr_t FunctionPointer;
    
    /* 
    ** Check to see if the task_id given is valid 
    */
    if (task_id >= OS_MAX_TASKS || OS_task_table[task_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    /*
    ** Call the thread Delete hook if there is one.
    */
    if ( OS_task_table[task_id].delete_hook_pointer != NULL)
    {
       FunctionPointer = (FuncPtr_t)(OS_task_table[task_id].delete_hook_pointer);
       (*FunctionPointer)();
    }

    /* 
    ** Try to delete the task 
    */
#ifdef _MAC_OS_
    /*
    ** Here, pthread_kill with a Signal handler is used, because pthread_cancel does not seem to
    ** kill the thread on OS X. 
    ** In addition, the OS Task Delete hook API can use this signal handler to call
    ** the users clean up function. 
    ** The use of the pthread_cleanup_push and pthread_cleanup_pop functions were investigated, 
    ** but these functions only work in the same logical function block.
    */    
    ret = pthread_kill(OS_task_table[task_id].id, SIGUSR2); 
#else
    ret = pthread_cancel(OS_task_table[task_id].id);
#endif
    if (ret != 0)
    {
        /*debugging statement only*/
        /*printf("FAILED PTHREAD CANCEL %d, %d \n",ret, ESRCH); */
        return OS_ERROR;
    }    
    
    /*
    ** Now that the task is deleted, remove its 
    ** "presence" in OS_task_table
    */
    pthread_mutex_lock(&OS_task_table_mut); 

    OS_task_table[task_id].free = TRUE;
    strcpy(OS_task_table[task_id].name, "");
    OS_task_table[task_id].creator = UNINITIALIZED;
    OS_task_table[task_id].stack_size = UNINITIALIZED;
    OS_task_table[task_id].priority = UNINITIALIZED;    
    OS_task_table[task_id].id = UNINITIALIZED;
    OS_task_table[task_id].delete_hook_pointer = NULL;
    
    pthread_mutex_unlock(&OS_task_table_mut);

    return OS_SUCCESS;
    
}/* end OS_TaskDelete */

/*--------------------------------------------------------------------------------------
     Name: OS_TaskExit

    Purpose: Exits the calling task and removes it from the OS_task_table.

    returns: Nothing 
---------------------------------------------------------------------------------------*/

void OS_TaskExit()
{
    uint32 task_id;

    task_id = OS_TaskGetId();

    pthread_mutex_lock(&OS_task_table_mut); 

    OS_task_table[task_id].free = TRUE;
    strcpy(OS_task_table[task_id].name, "");
    OS_task_table[task_id].creator = UNINITIALIZED;
    OS_task_table[task_id].stack_size = UNINITIALIZED;
    OS_task_table[task_id].priority = UNINITIALIZED;
    OS_task_table[task_id].id = UNINITIALIZED;
    OS_task_table[task_id].delete_hook_pointer = NULL;
    
    pthread_mutex_unlock(&OS_task_table_mut);

    pthread_exit(NULL);

}/*end OS_TaskExit */
/*---------------------------------------------------------------------------------------
   Name: OS_TaskDelay

   Purpose: Delay a task for specified amount of milliseconds

   returns: OS_ERROR if sleep fails or millisecond = 0
            OS_SUCCESS if success
---------------------------------------------------------------------------------------*/
int32 OS_TaskDelay(uint32 millisecond )
{
    if (usleep(millisecond * 1000 ) != 0)
    {
        return OS_ERROR;
    }
    else
    {
        return OS_SUCCESS;
    }
    
}/* end OS_TaskDelay */

/*---------------------------------------------------------------------------------------
   Name: OS_TaskSetPriority

   Purpose: Sets the given task to a new priority

    returns: OS_ERR_INVALID_ID if the ID passed to it is invalid
             OS_ERR_INVALID_PRIORITY if the priority is greater than the max 
             allowed
             OS_ERROR if the OS call to change the priority fails
             OS_SUCCESS if success
---------------------------------------------------------------------------------------*/
int32 OS_TaskSetPriority (uint32 task_id, uint32 new_priority)
{
    pthread_attr_t     custom_attr ;
    struct sched_param priority_holder ;
    int                os_priority;

    if(task_id >= OS_MAX_TASKS || OS_task_table[task_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    if (new_priority > MAX_PRIORITY)
    {
        return OS_ERR_INVALID_PRIORITY;
    }
   
    /* Change OSAL priority into a priority that will work for this OS */
    os_priority = OS_PriorityRemap(new_priority);

    /* 
    ** Set priority -- This is currently incomplete ..
    */
    priority_holder.sched_priority = os_priority ;
    if(pthread_attr_setschedparam(&custom_attr,&priority_holder))
    {
       printf("pthread_attr_setschedparam error in OS_TaskSetPriority, Task ID = %lu\n",task_id);
       return(OS_ERROR);
    }

    /* Use the abstracted priority, not the OS one */
    /* Change the priority in the table as well */
    OS_task_table[task_id].priority = new_priority;

   return OS_SUCCESS;
} /* end OS_TaskSetPriority */


/*---------------------------------------------------------------------------------------
   Name: OS_TaskRegister
  
   Purpose: Registers the calling task id with the task by adding the var to the tcb
            It searches the OS_task_table to find the task_id corresponding to the tcb_id
            
   Returns: OS_ERR_INVALID_ID if there the specified ID could not be found
            OS_ERROR if the OS call fails
            OS_SUCCESS if success
---------------------------------------------------------------------------------------*/
int32 OS_TaskRegister (void)
{
    int          i;
    int          ret;
    uint32       task_id;
    pthread_t    pthread_id;

#ifdef _MAC_OS_
    /* set up a signal handler to be able to cancel this task if needed */
    signal(SIGUSR2, OS_ThreadKillHandler);
#endif

    /* 
    ** Get PTHREAD Id
    */
    pthread_id = pthread_self();

    /*
    ** Look our task ID in table 
    */
    for(i = 0; i < OS_MAX_TASKS; i++)
    {
       if(OS_task_table[i].id == pthread_id)
       {
          break;
       }
    }
    task_id = i;

    if(task_id == OS_MAX_TASKS)
    {
        return OS_ERR_INVALID_ID;
    }

    /*
    ** Add pthread variable
    */
    ret = pthread_setspecific(thread_key, (void *)task_id);
    if ( ret != 0 )
    {
       printf("OS_TaskRegister Failed during pthread_setspecific function\n");
       return(OS_ERROR);
    }

    return OS_SUCCESS;
}/* end OS_TaskRegister */

/*---------------------------------------------------------------------------------------
   Name: OS_TaskGetId

   Purpose: This function returns the #defined task id of the calling task

   Notes: The OS_task_key is initialized by the task switch if AND ONLY IF the 
          OS_task_key has been registered via OS_TaskRegister(..).  If this is not 
          called prior to this call, the value will be old and wrong.
---------------------------------------------------------------------------------------*/
uint32 OS_TaskGetId (void)
{ 
   void*   task_id;
   int     task_id_int;
   uint32   task_key;
   task_key = 0;
   
   task_id = (void *)pthread_getspecific(thread_key);

   memcpy(& task_id_int,&task_id, sizeof(uint32));
   task_key = task_id_int & 0xFFFF;
   
   return(task_key);
}/* end OS_TaskGetId */

/*--------------------------------------------------------------------------------------
    Name: OS_TaskGetIdByName

    Purpose: This function tries to find a task Id given the name of a task

    Returns: OS_INVALID_POINTER if the pointers passed in are NULL
             OS_ERR_NAME_TOO_LONG if th ename to found is too long to begin with
             OS_ERR_NAME_NOT_FOUND if the name wasn't found in the table
             OS_SUCCESS if SUCCESS
---------------------------------------------------------------------------------------*/

int32 OS_TaskGetIdByName (uint32 *task_id, const char *task_name)
{
    uint32 i;

    if (task_id == NULL || task_name == NULL)
    {
       return OS_INVALID_POINTER;
    }
    
    /* 
    ** we don't want to allow names too long because they won't be found at all 
    */
    if (strlen(task_name) >= OS_MAX_API_NAME)
    {
       return OS_ERR_NAME_TOO_LONG;
    }

    for (i = 0; i < OS_MAX_TASKS; i++)
    {
        if((OS_task_table[i].free != TRUE) &&
                (strcmp(OS_task_table[i].name,(char*) task_name) == 0 ))
        {
            *task_id = i;
            return OS_SUCCESS;
        }
    }
    /* The name was not found in the table,
    **  or it was, and the task_id isn't valid anymore 
    */
    return OS_ERR_NAME_NOT_FOUND;

}/* end OS_TaskGetIdByName */            

/*---------------------------------------------------------------------------------------
    Name: OS_TaskGetInfo

    Purpose: This function will pass back a pointer to structure that contains 
             all of the relevant info (creator, stack size, priority, name) about the 
             specified task. 

    Returns: OS_ERR_INVALID_ID if the ID passed to it is invalid
             OS_INVALID_POINTER if the task_prop pointer is NULL
             OS_SUCCESS if it copied all of the relevant info over
 
---------------------------------------------------------------------------------------*/
int32 OS_TaskGetInfo (uint32 task_id, OS_task_prop_t *task_prop)  
{
    /* 
    ** Check to see that the id given is valid 
    */
    if (task_id >= OS_MAX_TASKS || OS_task_table[task_id].free == TRUE)
    {
       return OS_ERR_INVALID_ID;
    }

    if( task_prop == NULL)
    {
       return OS_INVALID_POINTER;
    }

    /* put the info into the stucture */
    pthread_mutex_lock(&OS_task_table_mut); 

    task_prop -> creator =    OS_task_table[task_id].creator;
    task_prop -> stack_size = OS_task_table[task_id].stack_size;
    task_prop -> priority =   OS_task_table[task_id].priority;
    task_prop -> OStask_id =  (uint32) OS_task_table[task_id].id;
    
    strcpy(task_prop-> name, OS_task_table[task_id].name);

    pthread_mutex_unlock(&OS_task_table_mut);
    
    return OS_SUCCESS;
    
} /* end OS_TaskGetInfo */

/*--------------------------------------------------------------------------------------
     Name: OS_TaskInstallDeleteHandler

    Purpose: Installs a handler for when the task is deleted.

    returns: status
---------------------------------------------------------------------------------------*/

int32 OS_TaskInstallDeleteHandler(void *function_pointer)
{
    uint32 task_id;

    task_id = OS_TaskGetId();

    if ( task_id >= OS_MAX_TASKS )
    {
       return(OS_ERR_INVALID_ID);
    }

    pthread_mutex_lock(&OS_task_table_mut); 

    if ( OS_task_table[task_id].free != FALSE )
    {
       /* 
       ** Somehow the calling task is not registered 
       */
       pthread_mutex_unlock(&OS_task_table_mut);
       return(OS_ERR_INVALID_ID);
    }

    /*
    ** Install the pointer
    */
    OS_task_table[task_id].delete_hook_pointer = function_pointer;    
    
    pthread_mutex_unlock(&OS_task_table_mut);

    return(OS_SUCCESS);
    
}/*end OS_TaskInstallDeleteHandler */

/****************************************************************************************
                                MESSAGE QUEUE API
****************************************************************************************/
#ifdef OSAL_SOCKET_QUEUE
/*---------------------------------------------------------------------------------------
   Name: OS_QueueCreate

   Purpose: Create a message queue which can be refered to by name or ID

   Returns: OS_INVALID_POINTER if a pointer passed in is NULL
            OS_ERR_NAME_TOO_LONG if the name passed in is too long
            OS_ERR_NO_FREE_IDS if there are already the max queues created
            OS_ERR_NAME_TAKEN if the name is already being used on another queue
            OS_ERROR if the OS create call fails
            OS_SUCCESS if success

   Notes: the flahs parameter is unused.
---------------------------------------------------------------------------------------*/
int32 OS_QueueCreate (uint32 *queue_id, const char *queue_name, uint32 queue_depth,
                       uint32 data_size, uint32 flags)
{
   int                     tmpSkt;
   int                     returnStat;
   struct sockaddr_in      servaddr;
   int                     i;
   uint32                  possible_qid;

    if ( queue_id == NULL || queue_name == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /* we don't want to allow names too long*/
    /* if truncated, two names might be the same */

    if (strlen(queue_name) >= OS_MAX_API_NAME)
    {
       return OS_ERR_NAME_TOO_LONG;
    }

   /* Check Parameters */

    pthread_mutex_lock(&OS_queue_table_mut);    
    
    for(possible_qid = 0; possible_qid < OS_MAX_QUEUES; possible_qid++)
    {
        if (OS_queue_table[possible_qid].free == TRUE)
            break;
    }
        
    if( possible_qid >= OS_MAX_QUEUES || OS_queue_table[possible_qid].free != TRUE)
    {
        pthread_mutex_unlock(&OS_queue_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }

    /* Check to see if the name is already taken */
    for (i = 0; i < OS_MAX_QUEUES; i++)
    {
        if ((OS_queue_table[i].free == FALSE) &&
                strcmp ((char*) queue_name, OS_queue_table[i].name) == 0)
        {
            pthread_mutex_unlock(&OS_queue_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    } 

    /* Set the possible task Id to not free so that
     * no other task can try to use it */

    OS_queue_table[possible_qid].free = FALSE;
    
    pthread_mutex_unlock(&OS_queue_table_mut);
    
    tmpSkt = socket(AF_INET, SOCK_DGRAM, 0);
    if ( tmpSkt == -1 )
    {
        pthread_mutex_lock(&OS_queue_table_mut);
        OS_queue_table[possible_qid].free = TRUE;
        pthread_mutex_unlock(&OS_queue_table_mut);

        printf("Failed to create a socket on OS_QueueCreate. errno = %d\n",errno);
        return OS_ERROR;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_port        = htons(OS_BASE_PORT + possible_qid);
    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); 

   /* 
   ** bind the input socket to a pipe
   ** port numbers are OS_BASE_PORT + queue_id
   */
   returnStat = bind(tmpSkt,(struct sockaddr *)&servaddr, sizeof(servaddr));
   
   if ( returnStat == -1 )
   {
        pthread_mutex_lock(&OS_queue_table_mut);
        OS_queue_table[possible_qid].free = TRUE;
        pthread_mutex_unlock(&OS_queue_table_mut);

        printf("bind failed on OS_QueueCreate. errno = %d\n",errno);
        return OS_ERROR;
   }
   
   /*
   ** store socket handle
   */
   *queue_id = possible_qid;
   
    pthread_mutex_lock(&OS_queue_table_mut);    

   OS_queue_table[*queue_id].id = tmpSkt;
   OS_queue_table[*queue_id].free = FALSE;
   strcpy( OS_queue_table[*queue_id].name, (char*) queue_name);
   OS_queue_table[*queue_id].creator = OS_FindCreator();

    pthread_mutex_unlock(&OS_queue_table_mut);

   return OS_SUCCESS;
    
}/* end OS_QueueCreate */


/*--------------------------------------------------------------------------------------
    Name: OS_QueueDelete

    Purpose: Deletes the specified message queue.

    Returns: OS_ERR_INVALID_ID if the id passed in does not exist
             OS_ERROR if the OS call to delete the queue fails 
             OS_SUCCESS if success

    Notes: If There are messages on the queue, they will be lost and any subsequent
           calls to QueueGet or QueuePut to this queue will result in errors
---------------------------------------------------------------------------------------*/
int32 OS_QueueDelete (uint32 queue_id)
{
    /* Check to see if the queue_id given is valid */
    
    if (queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    /* Try to delete the queue */

    if(close(OS_queue_table[queue_id].id) !=0)   
    {
        return OS_ERROR;
    }
        
    /* 
     * Now that the queue is deleted, remove its "presence"
     * in OS_message_q_table and OS_message_q_name_table 
    */
        
    pthread_mutex_lock(&OS_queue_table_mut);    

    OS_queue_table[queue_id].free = TRUE;
    strcpy(OS_queue_table[queue_id].name, "");
    OS_queue_table[queue_id].creator = UNINITIALIZED;
    OS_queue_table[queue_id].id = UNINITIALIZED;

    pthread_mutex_unlock(&OS_queue_table_mut);
 
   return OS_SUCCESS;

} /* end OS_QueueDelete */

/*---------------------------------------------------------------------------------------
   Name: OS_QueueGet

   Purpose: Receive a message on a message queue.  Will pend or timeout on the receive.
   Returns: OS_ERR_INVALID_ID if the given ID does not exist
            OS_INVALID_POINTER if a pointer passed in is NULL
            OS_QUEUE_EMPTY if the Queue has no messages on it to be recieved
            OS_QUEUE_TIMEOUT if the timeout was OS_PEND and the time expired
            OS_QUEUE_INVALID_SIZE if the size copied from the queue was not correct
            OS_ERROR if there was an error waiting for the timeout
            OS_SUCCESS if success
---------------------------------------------------------------------------------------*/
int32 OS_QueueGet (uint32 queue_id, void *data, uint32 size, uint32 *size_copied, int32 timeout)
{
   int sizeCopied;
   int flags;

   /*
   ** Check Parameters 
   */
   if(queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
   {
       return OS_ERR_INVALID_ID;
   }
   else if( (data == NULL) || (size_copied == NULL) )
   {
       return OS_INVALID_POINTER;
   }
    
   /*
   ** Read the socket for data
   */
   if (timeout == OS_PEND) 
   {      
      fcntl(OS_queue_table[queue_id].id,F_SETFL,0);
      /*
      ** A signal can interrupt the recvfrom call, so the call has to be done with 
      ** a loop
      */
      do 
      {
         sizeCopied = recvfrom(OS_queue_table[queue_id].id, data, size, 0, NULL, NULL);
      } while ( sizeCopied == -1 && errno == EINTR );
      
      if(sizeCopied != size )
      {
         *size_copied = 0;
         return(OS_QUEUE_INVALID_SIZE);
      }
   }
   else if (timeout == OS_CHECK)
   {      
      flags = fcntl(OS_queue_table[queue_id].id, F_GETFL, 0);
      fcntl(OS_queue_table[queue_id].id,F_SETFL,flags|O_NONBLOCK);
      
      sizeCopied = recvfrom(OS_queue_table[queue_id].id, data, size, 0, NULL, NULL);

      fcntl(OS_queue_table[queue_id].id,F_SETFL,flags);
      
      if (sizeCopied == -1 && errno == EWOULDBLOCK )
      {
         *size_copied = 0;
         return(OS_QUEUE_EMPTY);
      }
      else if(sizeCopied != size )
      {
         *size_copied = 0;
         return(OS_QUEUE_INVALID_SIZE);
      }

   }
   else /* timeout */ 
   {
      int    rv;
      int    sock = OS_queue_table[queue_id].id;
      struct timeval tv_timeout;
      fd_set fdset;

      FD_ZERO( &fdset );
      FD_SET( sock, &fdset );

      /*
      ** Translate the timeout value from milliseconds into 
      ** seconds and nanoseconds.
      */
      tv_timeout.tv_usec = (timeout % 1000) * 1000;
      tv_timeout.tv_sec = timeout / 1000;
      
      /*
      ** Use select to wait for data to come in on the socket
      ** TODO: If the select call is interrupted, the timeout should be 
      ** re-computed to avoid having to delay for the full time.
      **       
      */
      do 
      {
         FD_ZERO( &fdset );
         FD_SET( sock, &fdset );
         rv = select( sock+1, &fdset, NULL, NULL, &tv_timeout );
      } while ( rv == -1 && errno == EINTR );
      
      if( rv > 0 )
      {
         /* got a packet within the timeout */
         sizeCopied = recvfrom(OS_queue_table[queue_id].id, data, size, 0, NULL, NULL);

         if ( sizeCopied == size )
         {
            *size_copied = sizeCopied;
            return OS_SUCCESS;
         }
         else
         {
            *size_copied = 0;
            return OS_QUEUE_INVALID_SIZE;
         }
      }
      else if ( rv < 0 )
      {
         /* 
         ** Need to handle Select error codes here 
         ** This might need a new error code: OS_QUEUE_TIMEOUT_ERROR
         */
         printf("Bad return value from select: %d, sock = %d\n", rv, sock);
         return OS_ERROR;
      }
      
      /*
      ** If rv == 0, then the select timed out with no data
      */
      return(OS_QUEUE_TIMEOUT);
     
   } /* END timeout */

   /*
   ** Should never really get here.
   */
   return OS_SUCCESS;

} /* end OS_QueueGet */

/*---------------------------------------------------------------------------------------
   Name: OS_QueuePut

   Purpose: Put a message on a message queue.

   Returns: OS_ERR_INVALID_ID if the queue id passed in is not a valid queue
            OS_INVALID_POINTER if the data pointer is NULL
            OS_QUEUE_FULL if the queue cannot accept another message
            OS_ERROR if the OS call returns an error
            OS_SUCCESS if SUCCESS            
   
   Notes: The flags parameter is not used.  The message put is always configured to
            immediately return an error if the receiving message queue is full.
---------------------------------------------------------------------------------------*/
int32 OS_QueuePut (uint32 queue_id, void *data, uint32 size, uint32 flags)
{

   struct sockaddr_in serva;
   static int socketFlags = 0;
   int bytesSent    = 0;
   int tempSkt      = 0;

   /*
   ** Check Parameters 
   */
   if(queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
   {
       return OS_ERR_INVALID_ID;
   }
   if (data == NULL)
   {
       return OS_INVALID_POINTER;
   }

   /* 
   ** specify the IP addres and port number of destination
   */
   memset(&serva, 0, sizeof(serva));
   serva.sin_family      = AF_INET;
   serva.sin_port        = htons(OS_BASE_PORT + queue_id);
   serva.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   /*
   ** open a temporary socket to transfer the packet to MR
   */
   tempSkt = socket(AF_INET, SOCK_DGRAM, 0);

   /* 
   ** send the packet to the message router task (MR)
   */
   bytesSent = sendto(tempSkt,(char *)data, size, socketFlags,
                    (struct sockaddr *)&serva, sizeof(serva));
   if( bytesSent != size )
   {
      return(OS_QUEUE_FULL);
   }

   /* 
   ** close socket
   */
   close(tempSkt);

   return OS_SUCCESS;
} /* end OS_QueuePut */

#else

/* ---------------------- POSIX MESSAGE QUEUE IMPLEMENTATION ------------------------- */
/*---------------------------------------------------------------------------------------
 Name: OS_QueueCreate
 
 Purpose: Create a message queue which can be refered to by name or ID
 
 Returns: OS_INVALID_POINTER if a pointer passed in is NULL
 OS_ERR_NAME_TOO_LONG if the name passed in is too long
 OS_ERR_NO_FREE_IDS if there are already the max queues created
 OS_ERR_NAME_TAKEN if the name is already being used on another queue
 OS_ERROR if the OS create call fails
 OS_SUCCESS if success
 
 Notes: the flahs parameter is unused.
 ---------------------------------------------------------------------------------------*/
int32 OS_QueueCreate (uint32 *queue_id, const char *queue_name, uint32 queue_depth,
                      uint32 data_size, uint32 flags)
{
    int                     i;
    pid_t                   process_id;
    mqd_t                   queueDesc;
    struct mq_attr          queueAttr;   
    uint32                  possible_qid;
    char                    name[OS_MAX_API_NAME * 2];
    char                    process_id_string[OS_MAX_API_NAME+1];
    
    
    if ( queue_id == NULL || queue_name == NULL)
    {
        return OS_INVALID_POINTER;
    }
    
    /* we don't want to allow names too long*/
    /* if truncated, two names might be the same */
    
    if (strlen(queue_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }
    
    /* Check Parameters */
    
    pthread_mutex_lock(&OS_queue_table_mut);    
    
    for(possible_qid = 0; possible_qid < OS_MAX_QUEUES; possible_qid++)
    {
        if (OS_queue_table[possible_qid].free == TRUE)
            break;
    }
    
    if( possible_qid >= OS_MAX_QUEUES || OS_queue_table[possible_qid].free != TRUE)
    {
        pthread_mutex_unlock(&OS_queue_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }
    
    /* Check to see if the name is already taken */

    for (i = 0; i < OS_MAX_QUEUES; i++)
    {
        if ((OS_queue_table[i].free == FALSE) &&
            strcmp ((char*) queue_name, OS_queue_table[i].name) == 0)
        {
            pthread_mutex_unlock(&OS_queue_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    } 
    
    /* Set the possible task Id to not free so that
     * no other task can try to use it */
    
    OS_queue_table[possible_qid].free = FALSE;
    
    pthread_mutex_unlock(&OS_queue_table_mut);
    
    /* set queue attributes */
    queueAttr.mq_maxmsg  = 20;
    queueAttr.mq_msgsize = data_size;
   
    /*
    ** Construct the queue name:
    ** The name will consist of "/<process_id>.queue_name"
    */
 
    /* pre-pend / to queue name */
    strcpy(name, "/");

    /*
    ** Get the process ID 
    */
    process_id = getpid();
    sprintf(process_id_string, "%d", process_id);
    strcat(name, process_id_string);
    strcat(name,"."); 
    
    /*
    ** Add the name that was passed in
    */
    strcat(name, queue_name);
    
    /* 
    ** create message queue 
    */
    queueDesc = mq_open(name, O_CREAT | O_RDWR, 0666, &queueAttr);
    
    if ( queueDesc == -1 )
    {
        pthread_mutex_lock(&OS_queue_table_mut);
        OS_queue_table[possible_qid].free = TRUE;
        pthread_mutex_unlock(&OS_queue_table_mut);
        
        printf("OS_QueueCreate Error. errno = %d\n",errno);
        if( errno ==EINVAL)
        {
            printf("Your queue depth may be too large for the\n");
            printf("OS to handle. Please check the msg_max\n");
            printf("parameter located in /proc/sys/fs/mqueue/msg_max\n");
            printf("on your Linux file system and raise it if you\n");
            printf(" need to or run as root\n");
        }
        return OS_ERROR;
    }

    /*
    ** store queue_descriptor
    */
    *queue_id = possible_qid;
    
    pthread_mutex_lock(&OS_queue_table_mut);    
    
    OS_queue_table[*queue_id].id = queueDesc;
    OS_queue_table[*queue_id].free = FALSE;
    strcpy( OS_queue_table[*queue_id].name, (char*) queue_name);
    OS_queue_table[*queue_id].creator = OS_FindCreator();
    
    pthread_mutex_unlock(&OS_queue_table_mut);
    
    return OS_SUCCESS;
    
}/* end OS_QueueCreate */

/*--------------------------------------------------------------------------------------
 Name: OS_QueueDelete
 
 Purpose: Deletes the specified message queue.
 
 Returns: OS_ERR_INVALID_ID if the id passed in does not exist
 OS_ERROR if the OS call to delete the queue fails 
 OS_SUCCESS if success
 
 Notes: If There are messages on the queue, they will be lost and any subsequent
 calls to QueueGet or QueuePut to this queue will result in errors
 ---------------------------------------------------------------------------------------*/
int32 OS_QueueDelete (uint32 queue_id)
{
    pid_t   process_id;
    char    name[OS_MAX_API_NAME+1];
    char    process_id_string[OS_MAX_API_NAME+1];

    /* Check to see if the queue_id given is valid */
    
    if (queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
    {
       return OS_ERR_INVALID_ID;
    }
    
    /*
    ** Construct the queue name:
    ** The name will consist of "/<process_id>.queue_name"
    */

    /* pre-pend / to queue name */
    strcpy(name, "/");

    /*
    ** Get the process ID
    */
    process_id = getpid();
    sprintf(process_id_string, "%d", process_id);
    strcat(name, process_id_string);
    strcat(name,".");
    
    strcat(name, OS_queue_table[queue_id].name);
    
    /* Try to delete and unlink the queue */
    if((mq_close(OS_queue_table[queue_id].id) == -1) || (mq_unlink(name) == -1))
    {
        return OS_ERROR;
    }
    
    /* 
     * Now that the queue is deleted, remove its "presence"
     * in OS_message_q_table and OS_message_q_name_table 
     */
    pthread_mutex_lock(&OS_queue_table_mut);    
    
    OS_queue_table[queue_id].free = TRUE;
    strcpy(OS_queue_table[queue_id].name, "");
    OS_queue_table[queue_id].creator = UNINITIALIZED;
    OS_queue_table[queue_id].id = UNINITIALIZED;
    
    pthread_mutex_unlock(&OS_queue_table_mut);
    
    return OS_SUCCESS;
    
} /* end OS_QueueDelete */

/*---------------------------------------------------------------------------------------
 Name: OS_QueueGet
 
 Purpose: Receive a message on a message queue.  Will pend or timeout on the receive.
 Returns: OS_ERR_INVALID_ID if the given ID does not exist
 OS_ERR_INVALID_POINTER if a pointer passed in is NULL
 OS_QUEUE_EMPTY if the Queue has no messages on it to be recieved
 OS_QUEUE_TIMEOUT if the timeout was OS_PEND and the time expired
 OS_QUEUE_INVALID_SIZE if the size copied from the queue was not correct
 OS_SUCCESS if success
 ---------------------------------------------------------------------------------------*/
int32 OS_QueueGet (uint32 queue_id, void *data, uint32 size, uint32 *size_copied, int32 timeout)
{
    struct mq_attr  queueAttr;
    int             sizeCopied = -1;
    int             ret_val;
    struct timespec ts;
    
    /*
    ** Check Parameters 
    */
    if(queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
    else if( (data == NULL) || (size_copied == NULL) )
    {
        return OS_INVALID_POINTER;
    }
    
    /*
    ** Read the message queue for data
    */
    if (timeout == OS_PEND) 
    {      
        /*
        ** A signal can interrupt the mq_receive call, so the call has to be done with 
        ** a loop
        */
        do 
        {
           sizeCopied = mq_receive(OS_queue_table[queue_id].id, data, size, NULL);
        } while ( sizeCopied == -1 && errno == EINTR );

        if(sizeCopied != size )
        {
            *size_copied = 0;
            return(OS_QUEUE_INVALID_SIZE);
        }
    }
    else if (timeout == OS_CHECK)
    {      
        /* get queue attributes */
        if(mq_getattr(OS_queue_table[queue_id].id, &queueAttr))
        {
            return (OS_ERROR);
        }
        
        /* check how many messages in queue */
        if(queueAttr.mq_curmsgs) 
        {
            sizeCopied  = mq_receive(OS_queue_table[queue_id].id, data, size, NULL);
        } 
        else 
        {
            sizeCopied = -1;
        }
        
        if (sizeCopied == -1)
        {
            *size_copied = 0;
            return OS_QUEUE_EMPTY;
        }
        else if(sizeCopied != size )
        {
            *size_copied = 0;
            return(OS_QUEUE_INVALID_SIZE);
        }
    }
    else /* timeout */ 
    {
        ret_val = OS_CompAbsDelayTime( timeout , &ts) ;

        /*
        ** If the mq_timedreceive call is interrupted by a system call or signal,
        ** call it again.
        */
        do
        {
           sizeCopied = mq_timedreceive(OS_queue_table[queue_id].id, data, size, NULL, &ts);
        } while ( sizeCopied == -1 && errno == EINTR );
        
        if((sizeCopied == -1) && (errno == ETIMEDOUT))
        {
            return(OS_QUEUE_TIMEOUT);
        }
        else if( sizeCopied == size )
        {
            *size_copied = sizeCopied;
            return OS_SUCCESS;
        }
        else
        {
            *size_copied = 0;
            return OS_QUEUE_INVALID_SIZE;
        }
        
    } /* END timeout */
    
    /*
    ** Should never really get here.
    */
    return OS_SUCCESS;
    
} /* end OS_QueueGet */

/*---------------------------------------------------------------------------------------
 Name: OS_QueuePut
 
 Purpose: Put a message on a message queue.
 
 Returns: OS_ERR_INVALID_ID if the queue id passed in is not a valid queue
 OS_INVALID_POINTER if the data pointer is NULL
 OS_QUEUE_FULL if the queue cannot accept another message
 OS_ERROR if the OS call returns an error
 OS_SUCCESS if SUCCESS            
 
 Notes: The flags parameter is not used.  The message put is always configured to
 immediately return an error if the receiving message queue is full.
 ---------------------------------------------------------------------------------------*/
int32 OS_QueuePut (uint32 queue_id, void *data, uint32 size, uint32 flags)
{
    struct mq_attr  queueAttr;
    
    /*
    ** Check Parameters 
    */
    if(queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
    {
       return OS_ERR_INVALID_ID;
    }
    
    if (data == NULL)
    {
       return OS_INVALID_POINTER;
    }
    
    /* get queue attributes */
    if(mq_getattr(OS_queue_table[queue_id].id, &queueAttr))
    {
       return (OS_ERROR);
    }
    
    /* check if queue is full */
    if(queueAttr.mq_curmsgs >= queueAttr.mq_maxmsg) 
    {
       return(OS_QUEUE_FULL);
    }
    
    /* send message */
    if(mq_send(OS_queue_table[queue_id].id, data, size, 1) == -1) 
    {
        return(OS_ERROR);
    }
    
    return OS_SUCCESS;

} /* end OS_QueuePut */


/* --------------------- END POSIX MESSAGE QUEUE IMPLEMENTATION ---------------------- */
#endif

/*--------------------------------------------------------------------------------------
    Name: OS_QueueGetIdByName

    Purpose: This function tries to find a queue Id given the name of the queue. The
             id of the queue is passed back in queue_id

    Returns: OS_INVALID_POINTER if the name or id pointers are NULL
             OS_ERR_NAME_TOO_LONG the name passed in is too long
             OS_ERR_NAME_NOT_FOUND the name was not found in the table
             OS_SUCCESS if success
             
---------------------------------------------------------------------------------------*/

int32 OS_QueueGetIdByName (uint32 *queue_id, const char *queue_name)
{
    uint32 i;

    if(queue_id == NULL || queue_name == NULL)
    {
       return OS_INVALID_POINTER;
    }
    
    /* a name too long wouldn't have been allowed in the first place
     * so we definitely won't find a name too long*/
 
    if (strlen(queue_name) >= OS_MAX_API_NAME)
    {
       return OS_ERR_NAME_TOO_LONG;
    }

    for (i = 0; i < OS_MAX_QUEUES; i++)
    {
        if (OS_queue_table[i].free != TRUE &&
           (strcmp(OS_queue_table[i].name, (char*) queue_name) == 0 ))
        {
            *queue_id = i;
            return OS_SUCCESS;
        }
    }

    /* The name was not found in the table,
     *  or it was, and the queue_id isn't valid anymore */
    return OS_ERR_NAME_NOT_FOUND;

}/* end OS_QueueGetIdByName */

/*---------------------------------------------------------------------------------------
    Name: OS_QueueGetInfo

    Purpose: This function will pass back a pointer to structure that contains 
             all of the relevant info (name and creator) about the specified queue. 

    Returns: OS_INVALID_POINTER if queue_prop is NULL
             OS_ERR_INVALID_ID if the ID given is not  a valid queue
             OS_SUCCESS if the info was copied over correctly
---------------------------------------------------------------------------------------*/
int32 OS_QueueGetInfo (uint32 queue_id, OS_queue_prop_t *queue_prop)  
{
    /* Check to see that the id given is valid */
    
    if (queue_prop == NULL)
    {
        return OS_INVALID_POINTER;
    }
    
    if (queue_id >= OS_MAX_QUEUES || OS_queue_table[queue_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    /* put the info into the stucture */
    pthread_mutex_lock(&OS_queue_table_mut);    

    queue_prop -> creator =   OS_queue_table[queue_id].creator;
    strcpy(queue_prop -> name, OS_queue_table[queue_id].name);

    pthread_mutex_unlock(&OS_queue_table_mut);

    return OS_SUCCESS;
    
} /* end OS_QueueGetInfo */

/****************************************************************************************
                                  SEMAPHORE API
****************************************************************************************/

/*---------------------------------------------------------------------------------------
   Name: OS_BinSemCreate

   Purpose: Creates a binary semaphore with initial value specified by
            sem_initial_value and name specified by sem_name. sem_id will be 
            returned to the caller
            
   Returns: OS_INVALID_POINTER if sen name or sem_id are NULL
            OS_ERR_NAME_TOO_LONG if the name given is too long
            OS_ERR_NO_FREE_IDS if all of the semaphore ids are taken
            OS_ERR_NAME_TAKEN if this is already the name of a binary semaphore
            OS_SEM_FAILURE if the OS call failed
            OS_SUCCESS if success
            

   Notes: options is an unused parameter 
---------------------------------------------------------------------------------------*/
int32 OS_BinSemCreate (uint32 *sem_id, const char *sem_name, uint32 sem_initial_value,
                        uint32 options)
{
    uint32 possible_semid;
    uint32 i;
#ifdef _MAC_OS_
    char   SemName [OS_MAX_API_NAME];
#else
    int Status;
#endif

    if (sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }
    
    /* we don't want to allow names too long*/
    /* if truncated, two names might be the same */
    
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    /* Check Parameters */

    pthread_mutex_lock(&OS_bin_sem_table_mut);  

    for (possible_semid = 0; possible_semid < OS_MAX_BIN_SEMAPHORES; possible_semid++)
    {
        if (OS_bin_sem_table[possible_semid].free == TRUE)    
            break;
    }

    if((possible_semid >= OS_MAX_BIN_SEMAPHORES) ||  
       (OS_bin_sem_table[possible_semid].free != TRUE))
    {
        pthread_mutex_unlock(&OS_bin_sem_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }
    
    /* Check to see if the name is already taken */
    for (i = 0; i < OS_MAX_BIN_SEMAPHORES; i++)
    {
        if ((OS_bin_sem_table[i].free == FALSE) &&
                strcmp ((char*) sem_name, OS_bin_sem_table[i].name) == 0)
        {
            pthread_mutex_unlock(&OS_bin_sem_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    }  

    /* Set the ID to be taken so another task doesn't try to grab it */
    OS_bin_sem_table[possible_semid].free = FALSE;

    pthread_mutex_unlock(&OS_bin_sem_table_mut);

    /*
    ** Create semaphore
    */
    errno = 0;
   
    /* Check to make sure the sem value is going to be either 0 or 1 */
    if (sem_initial_value < 0)
    {
        sem_initial_value = 0;
    }
    else
    {
        if (sem_initial_value > 1)
        {
            sem_initial_value = 1;
        }
    }

#ifdef _MAC_OS_
    sprintf(SemName,"OS_BinSemName%lu",possible_semid);
    /* unlink the name first, to make sure it doesn't exist anywhere*/
    sem_unlink(SemName);
    OS_bin_sem_table[possible_semid].id = sem_open(SemName,O_CREAT, 666, sem_initial_value);
    if(OS_bin_sem_table[possible_semid].id == (sem_t*) SEM_FAILED )
    {        
       /* Since the call failed, set the free flag back to true */
       pthread_mutex_lock(&OS_bin_sem_table_mut);        
       OS_bin_sem_table[possible_semid].free = TRUE;
       pthread_mutex_unlock(&OS_bin_sem_table_mut);
       printf("Error Creating semaphore in OS_BinSemCreate! errno = %d\n", errno); 
       return OS_SEM_FAILURE;
    }
#else /* _LINUX_OS_ */
    Status = sem_init( &(OS_bin_sem_table[possible_semid].id) ,0 , sem_initial_value);
    if( Status == -1 )
    {
        /* Since the call failed, set the free flag back to true */
       pthread_mutex_lock(&OS_bin_sem_table_mut);        
       OS_bin_sem_table[possible_semid].free = TRUE;
       pthread_mutex_unlock(&OS_bin_sem_table_mut);

       printf("Error Creating semaphore in OS_BinSemCreate! errno = %d\n", errno);
       return OS_SEM_FAILURE;
    }
#endif
            
    *sem_id = possible_semid;

    pthread_mutex_lock(&OS_bin_sem_table_mut);  

    strcpy(OS_bin_sem_table[*sem_id].name , (char*) sem_name);
    OS_bin_sem_table[*sem_id].creator = OS_FindCreator();
    
    OS_bin_sem_table[*sem_id].max_value = 1;
    OS_bin_sem_table[*sem_id].current_value = sem_initial_value;
    
    pthread_mutex_unlock(&OS_bin_sem_table_mut);

    return OS_SUCCESS;
}/* end OS_BinSemCreate */

/*--------------------------------------------------------------------------------------
     Name: OS_BinSemDelete

    Purpose: Deletes the specified Binary Semaphore.

    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid binary semaphore
             OS_SEM_FAILURE the OS call failed
             OS_SUCCESS if success
    
    Notes: Since we can't delete a semaphore which is currently locked by some task 
           (as it may ber crucial to completing the task), the semaphore must be full to
           allow deletion.
---------------------------------------------------------------------------------------*/
int32 OS_BinSemDelete (uint32 sem_id)
{
    /* Check to see if this sem_id is valid */
    if (sem_id >= OS_MAX_BIN_SEMAPHORES || OS_bin_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

#ifdef _MAC_OS_
    if (sem_close( (OS_bin_sem_table[sem_id].id)) != 0) /* 0 = success */ 
#else /* _LINUX_OS_ */       
    if (sem_destroy( &(OS_bin_sem_table[sem_id].id)) != 0) /* 0 = success */ 
#endif
    {
        return OS_SEM_FAILURE;
    }

    /* Remove the Id from the table, and its name, so that it cannot be found again */
    pthread_mutex_lock(&OS_bin_sem_table_mut);  
   
    OS_bin_sem_table[sem_id].free = TRUE;
    strcpy(OS_bin_sem_table[sem_id].name , "");
    OS_bin_sem_table[sem_id].creator = UNINITIALIZED;
    OS_bin_sem_table[sem_id].max_value = 0;
    OS_bin_sem_table[sem_id].current_value = 0;

    pthread_mutex_unlock(&OS_bin_sem_table_mut);
   
    return OS_SUCCESS;

}/* end OS_BinSemDelete */


/*---------------------------------------------------------------------------------------
    Name: OS_BinSemGive

    Purpose: The function  unlocks the semaphore referenced by sem_id by performing
             a semaphore unlock operation on that semaphore.If the semaphore value 
             resulting from this operation is positive, then no threads were blocked             
             waiting for the semaphore to become unlocked; the semaphore value is
             simply incremented for this semaphore.

    
    Returns: OS_SEM_FAILURE the semaphore was not previously  initialized or is not
             in the array of semaphores defined by the system
             OS_ERR_INVALID_ID if the id passed in is not a binary semaphore
             OS_SUCCESS if success
                
---------------------------------------------------------------------------------------*/

int32 OS_BinSemGive ( uint32 sem_id )
{
    uint32 ret_val ;
    int32    ret;
   
    /* Check Parameters */
    if(sem_id >= OS_MAX_BIN_SEMAPHORES || OS_bin_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
    
    /* If the semaphore isn't totally full, perform the give */
    if ( OS_bin_sem_table[sem_id].current_value  < OS_bin_sem_table[sem_id].max_value )
    {
        #ifdef _MAC_OS_
           ret = sem_post(OS_bin_sem_table[sem_id].id);
        #else
           ret = sem_post(&OS_bin_sem_table[sem_id].id);
        #endif
        if ( ret != 0 )
        {
            ret_val = OS_SEM_FAILURE;
        }
        else
        {
            ret_val = OS_SUCCESS ;
            OS_bin_sem_table[sem_id].current_value ++;
        }
    }
    else /* drop the post call */
    {
        ret_val = OS_SUCCESS ;
    }
    
    return ret_val;
}/* end OS_BinSemGive */

/*---------------------------------------------------------------------------------------
    Name: OS_BinSemFlush

    Purpose: The function unblocks all tasks pending on the specified semaphore. However,
             this function does not change the state of the semaphore.

    
    Returns: OS_SEM_FAILURE the semaphore was not previously  initialized or is not
             in the array of semaphores defined by the system
             OS_ERR_INVALID_ID if the id passed in is not a binary semaphore
             OS_SUCCESS if success

---------------------------------------------------------------------------------------*/
int32 OS_BinSemFlush (uint32 sem_id)
{
    uint32 ret_val ;
    int32    ret = 0;
    int i;

    /* Check Parameters */
    if(sem_id >= OS_MAX_BIN_SEMAPHORES || OS_bin_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
    
    for (i = OS_bin_sem_table[sem_id].current_value  ; i < 0; i++)
    {
         #ifdef _MAC_OS_ 
            sem_post(OS_bin_sem_table[sem_id].id);
         #else      
            sem_post(&(OS_bin_sem_table[sem_id].id));
         #endif
    }
    
    
    if ( ret != 0 )
    {
       ret_val = OS_SEM_FAILURE;
    }
    else
    {
       ret_val = OS_SUCCESS ;
    }
    
    return(ret_val);

}/* end OS_BinSemFlush */

/*---------------------------------------------------------------------------------------
    Name:    OS_BinSemTake

    Purpose: The locks the semaphore referenced by sem_id by performing a 
             semaphore lock operation on that semaphore.If the semaphore value 
             is currently zero, then the calling thread shall not return from 
             the call until it either locks the semaphore or the call is 
             interrupted by a signal.

    Return:  OS_ERR_INVALID_ID the Id passed in is not a valid binary semaphore
             OS_SEM_FAILURE if the OS call failed
             OS_SUCCESS if success
             
----------------------------------------------------------------------------------------*/

int32 OS_BinSemTake ( uint32 sem_id )
{
    uint32 ret_val ;
    int    ret = -1;
    
    if(sem_id >= OS_MAX_BIN_SEMAPHORES  || OS_bin_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
    
    /* decrement in anticipation of a good take, because if it is, the task may block and 
     it won't be able to decrement the counter */
    OS_bin_sem_table[sem_id].current_value --;
    
    /*
    ** A signal can interrupt the sem_wait call, so the call has to be done with 
    ** a loop
    */
    do 
    {
       #ifdef _MAC_OS_
          ret = sem_wait(OS_bin_sem_table[sem_id].id);
       #else 
          ret = sem_wait(&OS_bin_sem_table[sem_id].id);
       #endif
    } while ( ret == -1 && errno == EINTR );
    
    if ( ret == 0 )
    {
        ret_val = OS_SUCCESS;
    }
    else
    {
        /* undo the premature decrement */
        OS_bin_sem_table[sem_id].current_value ++;
        ret_val = OS_SEM_FAILURE;
    }
    
    return ret_val;
}/* end OS_BinSemTake */

/*---------------------------------------------------------------------------------------
    Name: OS_BinSemTimedWait
    
    Purpose: The function locks the semaphore referenced by sem_id . However,
             if the semaphore cannot be locked without waiting for another process
             or thread to unlock the semaphore , this wait shall be terminated when 
             the specified timeout ,msecs, expires.

    Returns: OS_SEM_TIMEOUT if semaphore was not relinquished in time
             OS_SUCCESS if success
             OS_SEM_FAILURE the semaphore was not previously initialized or is not
             in the array of semaphores defined by the system
             OS_ERR_INVALID_ID if the ID passed in is not a valid semaphore ID

    Notes:
             Fix provided by Joshua M. Eliser 
----------------------------------------------------------------------------------------*/

#ifdef _MAC_OS_ 
/*
** The Mac OS X version must do a busy loop trying to see if the semaphore is available
*/
   int32 OS_BinSemTimedWait ( uint32 sem_id, uint32 msecs )
   {
       uint32           ret_val ;
       int              timeloop;

       if( (sem_id >= OS_MAX_BIN_SEMAPHORES) || (OS_bin_sem_table[sem_id].free == TRUE) )
       {
           return OS_ERR_INVALID_ID;
       }

       /* 
       ** Simulate the timeout with a sleep loop 
       */
       OS_bin_sem_table[sem_id].current_value --;
       for (timeloop = msecs; timeloop >0; timeloop -= 100)
       {
           if((ret_val = sem_trywait(OS_bin_sem_table[sem_id].id)) == 0)
           {
              return OS_SUCCESS;
           }
           else if(errno == EAGAIN)
           {
              /* sleep for 100 msecs */
              usleep(100*1000);
           }
           else
           {
              /* 
              ** something besides the sem being taken made it fail 
              */
              OS_bin_sem_table[sem_id].current_value ++;
              return OS_SEM_FAILURE;
           }
       }
 
       OS_bin_sem_table[sem_id].current_value++;
       return OS_SEM_TIMEOUT;
   }

#else
/*
** The Linux version can use the sem_timedwait call
*/
   int32 OS_BinSemTimedWait ( uint32 sem_id, uint32 msecs )
   {
       uint32           ret_val;
       struct timespec  ts;
       int              sem_stat;

       if( (sem_id >= OS_MAX_BIN_SEMAPHORES) || (OS_bin_sem_table[sem_id].free == TRUE) )
       {
          return OS_ERR_INVALID_ID;
       }

       OS_bin_sem_table[sem_id].current_value --;

       /*
       ** Compute an absolute time for the delay
       */
       ret_val = OS_CompAbsDelayTime( msecs , &ts);

       /*
       ** If the pend gets interrupted by a system call, then re-enter the wait
       */
       do
       {
           sem_stat = sem_timedwait(&OS_bin_sem_table[sem_id].id, &ts);
       } while ( sem_stat == -1 && errno == EINTR );

       if ( sem_stat == 0 )
       {
            return(OS_SUCCESS);
       }
       else if ( sem_stat == -1 && errno == ETIMEDOUT ) 
       {
          /* Time out */ 
          OS_bin_sem_table[sem_id].current_value++;
          return(OS_SEM_TIMEOUT);
       }

       /* 
       ** something besides the sem being taken made it fail 
       */
       OS_bin_sem_table[sem_id].current_value ++;
       return OS_SEM_FAILURE;
   }
#endif
/*--------------------------------------------------------------------------------------
    Name: OS_BinSemGetIdByName

    Purpose: This function tries to find a binary sem Id given the name of a bin_sem
             The id is returned through sem_id

    Returns: OS_INVALID_POINTER is semid or sem_name are NULL pointers
             OS_ERR_NAME_TOO_LONG if the name given is to long to have been stored
             OS_ERR_NAME_NOT_FOUND if the name was not found in the table
             OS_SUCCESS if success
             
---------------------------------------------------------------------------------------*/
int32 OS_BinSemGetIdByName (uint32 *sem_id, const char *sem_name)
{
    uint32 i;

    if (sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /* a name too long wouldn't have been allowed in the first place
     * so we definitely won't find a name too long*/
    
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
       return OS_ERR_NAME_TOO_LONG;
    }

    for (i = 0; i < OS_MAX_BIN_SEMAPHORES; i++)
    {
        if (OS_bin_sem_table[i].free != TRUE &&
                (strcmp (OS_bin_sem_table[i].name , (char*) sem_name) == 0))
        {
            *sem_id = i;
            return OS_SUCCESS;
        }
    }
    /* The name was not found in the table,
     *  or it was, and the sem_id isn't valid anymore */

    return OS_ERR_NAME_NOT_FOUND;
    
}/* end OS_BinSemGetIdByName */
/*---------------------------------------------------------------------------------------
    Name: OS_BinSemGetInfo

    Purpose: This function will pass back a pointer to structure that contains 
             all of the relevant info( name and creator) about the specified binary
             semaphore.
             
    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid semaphore 
             OS_INVALID_POINTER if the bin_prop pointer is null
             OS_SUCCESS if success
---------------------------------------------------------------------------------------*/

int32 OS_BinSemGetInfo (uint32 sem_id, OS_bin_sem_prop_t *bin_prop)  
{
    /* Check to see that the id given is valid */
    
    if (sem_id >= OS_MAX_BIN_SEMAPHORES || OS_bin_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    if (bin_prop == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /* put the info into the stucture */
    pthread_mutex_lock(&OS_bin_sem_table_mut);  

    bin_prop ->creator =    OS_bin_sem_table[sem_id].creator;
    bin_prop -> value = OS_bin_sem_table[sem_id].current_value ;
    strcpy(bin_prop-> name, OS_bin_sem_table[sem_id].name);
    
    pthread_mutex_unlock(&OS_bin_sem_table_mut);

    return OS_SUCCESS;
    
} /* end OS_BinSemGetInfo */

/*---------------------------------------------------------------------------------------
   Name: OS_CountSemCreate

   Purpose: Creates a counting semaphore with initial value specified by
            sem_initial_value and name specified by sem_name. sem_id will be 
            returned to the caller
            
   Returns: OS_INVALID_POINTER if sen name or sem_id are NULL
            OS_ERR_NAME_TOO_LONG if the name given is too long
            OS_ERR_NO_FREE_IDS if all of the semaphore ids are taken
            OS_ERR_NAME_TAKEN if this is already the name of a counting semaphore
            OS_SEM_FAILURE if the OS call failed
            OS_INVALID_SEM_VALUE if the semaphore value is too high
            OS_SUCCESS if success
            

   Notes: options is an unused parameter 
---------------------------------------------------------------------------------------*/
int32 OS_CountSemCreate (uint32 *sem_id, const char *sem_name, uint32 sem_initial_value,
                        uint32 options)
{
    uint32 possible_semid;
    uint32 i;
#ifdef _MAC_OS_
    char   SemName [OS_MAX_API_NAME];
#else
    int Status;
#endif

    if (sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }
   
    /*
    ** Verify that the semaphore maximum value is not too high
    */
    if ( sem_initial_value > SEM_VALUE_MAX )
    {
        return OS_INVALID_SEM_VALUE;
    }
 
    /* 
    ** we don't want to allow names too long
    ** if truncated, two names might be the same 
    */
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  

    for (possible_semid = 0; possible_semid < OS_MAX_COUNT_SEMAPHORES; possible_semid++)
    {
        if (OS_count_sem_table[possible_semid].free == TRUE)    
            break;
    }
    
    if((possible_semid >= OS_MAX_COUNT_SEMAPHORES) ||  
       (OS_count_sem_table[possible_semid].free != TRUE))
    {
        /*
        ** Unlock
        */
        pthread_mutex_unlock(&OS_count_sem_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }
    
    /* Check to see if the name is already taken */
    for (i = 0; i < OS_MAX_COUNT_SEMAPHORES; i++)
    {
        if ((OS_count_sem_table[i].free == FALSE) &&
                strcmp ((char*) sem_name, OS_count_sem_table[i].name) == 0)
        {
            /*
            ** Unlock
            */
            pthread_mutex_unlock(&OS_count_sem_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    }   

    /* 
    ** set the ID to taken so no other task can grab it 
    */
    OS_count_sem_table[possible_semid].free = FALSE;


    /*
    ** Unlock
    */
    pthread_mutex_unlock(&OS_count_sem_table_mut);

    /*
    ** Create semaphore
    */
    errno = 0;
    
#ifdef _MAC_OS_
   sprintf(SemName,"OS_CountSemName%lu",possible_semid);
   /* unlink the name first, to make sure it doesn't exist anywhere*/
   sem_unlink(SemName);
   
   OS_count_sem_table[possible_semid].id = sem_open(SemName,O_CREAT, 666, sem_initial_value);
   if(OS_count_sem_table[possible_semid].id == (sem_t *)SEM_FAILED )
   {
       /* Since the call failed, set it the free flag back to true */

      /*
      ** Lock
      */
      pthread_mutex_lock(&OS_count_sem_table_mut); 

      OS_count_sem_table[possible_semid].free = TRUE;

      /*
      ** Unlock
      */
      pthread_mutex_unlock(&OS_count_sem_table_mut); 
      printf("Error Creating semaphore in OS_CountSemCreate! errno = %d\n", errno);
      return OS_SEM_FAILURE;
   }
#else
    Status = sem_init( &(OS_count_sem_table[possible_semid].id) ,0 , sem_initial_value);
    if( Status == -1 )
    {
      /* Since the call failed, set it the free flag back to true */

      /*
      ** Lock
      */
      pthread_mutex_lock(&OS_count_sem_table_mut); 
      OS_count_sem_table[possible_semid].free = TRUE;

      /*
      ** Unlock
      */
      pthread_mutex_unlock(&OS_count_sem_table_mut); 
      printf("Error Creating semaphore in OS_CountSemCreate! errno = %d\n", errno);
      return OS_SEM_FAILURE;
    }
#endif

    *sem_id = possible_semid;

    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  

    OS_count_sem_table[*sem_id].free = FALSE;
    strcpy(OS_count_sem_table[*sem_id].name , (char*) sem_name);
    OS_count_sem_table[*sem_id].creator = OS_FindCreator();
    OS_count_sem_table[*sem_id].max_value = SEM_VALUE_MAX; /* Linux semaphore max value */
    OS_count_sem_table[*sem_id].current_value = sem_initial_value;
    
    /*
    ** Unlock
    */
    pthread_mutex_unlock(&OS_count_sem_table_mut);

    return OS_SUCCESS;

}/* end OS_CountSemCreate */

/*--------------------------------------------------------------------------------------
     Name: OS_CountSemDelete

    Purpose: Deletes the specified Countary Semaphore.

    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid counting semaphore
             OS_SEM_FAILURE the OS call failed
             OS_SUCCESS if success
    
    Notes: Since we can't delete a semaphore which is currently locked by some task 
           (as it may ber crucial to completing the task), the semaphore must be full to
           allow deletion.
---------------------------------------------------------------------------------------*/

int32 OS_CountSemDelete (uint32 sem_id)
{
    /* 
    ** Check to see if this sem_id is valid 
    */
    if (sem_id >= OS_MAX_COUNT_SEMAPHORES || OS_count_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }   

    #ifdef _MAC_OS_
       if (sem_close( (OS_count_sem_table[sem_id].id)) != 0) /* 0 = success */ 
    #else
       if (sem_destroy( &(OS_count_sem_table[sem_id].id)) != 0) /* 0 = success */
    #endif
    {
        return OS_SEM_FAILURE;
    }

    /* 
    ** Remove the Id from the table, and its name, so that it cannot be found again 
    */

    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  
   
    OS_count_sem_table[sem_id].free = TRUE;
    strcpy(OS_count_sem_table[sem_id].name , "");
    OS_count_sem_table[sem_id].creator = UNINITIALIZED;
    OS_count_sem_table[sem_id].max_value = 0;
    OS_count_sem_table[sem_id].current_value = 0;

    /* 
    ** Unlock
    */
    pthread_mutex_unlock(&OS_count_sem_table_mut);
   
    return OS_SUCCESS;

}/* end OS_CountSemDelete */

/*---------------------------------------------------------------------------------------
    Name: OS_CountSemGive

    Purpose: The function  unlocks the semaphore referenced by sem_id by performing
             a semaphore unlock operation on that semaphore.If the semaphore value 
             resulting from this operation is positive, then no threads were blocked             
             waiting for the semaphore to become unlocked; the semaphore value is
             simply incremented for this semaphore.

    
    Returns: OS_SEM_FAILURE the semaphore was not previously  initialized or is not
             in the array of semaphores defined by the system
             OS_ERR_INVALID_ID if the id passed in is not a counting semaphore
             OS_SUCCESS if success
                
---------------------------------------------------------------------------------------*/

int32 OS_CountSemGive ( uint32 sem_id )
{
    int32  ret_val = OS_SUCCESS;
    int32  ret;
    
    /* 
    ** Check Parameters 
    */
    if(sem_id >= OS_MAX_COUNT_SEMAPHORES || OS_count_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
    
    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  

    /* 
    ** If the semaphore isn't totally full, perform the give 
    */
    if ( OS_count_sem_table[sem_id].current_value  < OS_count_sem_table[sem_id].max_value )
    {
        #ifdef _MAC_OS_
           ret = sem_post(OS_count_sem_table[sem_id].id);
        #else
           ret = sem_post(&OS_count_sem_table[sem_id].id);
        #endif
        if ( ret != 0 )
        {
            ret_val = OS_SEM_FAILURE;
        }
        else
        {
            ret_val = OS_SUCCESS ;
            OS_count_sem_table[sem_id].current_value ++;
        }
    }
    else /* drop the post call */
    {
        ret_val = OS_SUCCESS ;
    }

    /* 
    ** Unlock
    */
    pthread_mutex_unlock(&OS_count_sem_table_mut);

    return(ret_val);

}/* end OS_CountSemGive */

/*---------------------------------------------------------------------------------------
    Name:    OS_CountSemTake

    Purpose: The locks the semaphore referenced by sem_id by performing a 
             semaphore lock operation on that semaphore.If the semaphore value 
             is currently zero, then the calling thread shall not return from 
             the call until it either locks the semaphore or the call is 
             interrupted by a signal.

    Return:  OS_ERR_INVALID_ID the Id passed in is not a valid counting semaphore
             OS_SEM_FAILURE if the OS call failed
             OS_SUCCESS if success
             
----------------------------------------------------------------------------------------*/

int32 OS_CountSemTake ( uint32 sem_id )
{
    int32  ret_val = OS_SUCCESS;
    int    ret = -1;
    
    /* 
    ** Check Parameters 
    */
    if(sem_id >= OS_MAX_COUNT_SEMAPHORES  || OS_count_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    } 

    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  

    /* 
    ** decrement in anticipation of a good take, because if it is, the task may block and 
    **  it won't be able to decrement the counter 
    */
    OS_count_sem_table[sem_id].current_value --;

    /*
    ** A signal can interrupt the sem_wait call, so the call has to be done with     
    ** a loop
    */
    do   
    {
       #ifdef _MAC_OS_
          ret = sem_wait(OS_count_sem_table[sem_id].id);    
       #else   
          ret = sem_wait(&OS_count_sem_table[sem_id].id);
       #endif
    } while ( ret == -1 && errno == EINTR );

    if ( ret == 0 )
    {
        ret_val = OS_SUCCESS;
    }
    else
    {
        /* undo the premature decrement */
        OS_count_sem_table[sem_id].current_value ++;
        ret_val = OS_SEM_FAILURE;
    }
    
    /* 
    ** Unlock
    */
    pthread_mutex_unlock(&OS_count_sem_table_mut);

    return ret_val;

}/* end OS_CountSemTake */

/*---------------------------------------------------------------------------------------
    Name: OS_CountSemTimedWait
    
    Purpose: The function locks the semaphore referenced by sem_id . However,
             if the semaphore cannot be locked without waiting for another process
             or thread to unlock the semaphore , this wait shall be terminated when 
             the specified timeout ,msecs, expires.

    Returns: OS_SEM_TIMEOUT if semaphore was not relinquished in time
             OS_SUCCESS if success
             OS_SEM_FAILURE the semaphore was not previously initialized or is not
             in the array of semaphores defined by the system
             OS_ERR_INVALID_ID if the ID passed in is not a valid semaphore ID

----------------------------------------------------------------------------------------*/

#ifdef _MAC_OS_
   int32 OS_CountSemTimedWait ( uint32 sem_id, uint32 msecs )
   {
       int32 ret_val = OS_SUCCESS;
       int   timeloop;

       /* 
       ** Check Parameters 
       */
       if( (sem_id >= OS_MAX_COUNT_SEMAPHORES) || (OS_count_sem_table[sem_id].free == TRUE) )
       {
           return OS_ERR_INVALID_ID;   
       }

       /*
       ** Lock
       */
       pthread_mutex_lock(&OS_count_sem_table_mut);  

       /* premature decrement, just like regurlar SemTake */
       OS_count_sem_table[sem_id].current_value --;

       for (timeloop = msecs; timeloop >0; timeloop -= 100)
       {
          if((ret_val = sem_trywait(OS_count_sem_table[sem_id].id)) == 0)
          {
             ret_val =  OS_SUCCESS;
             break;
          }
          else if(errno == EAGAIN)
          {
              /* sleep for 100 msecs */
              usleep(100*1000);
          }
          else
          {
             /* 
             ** something besides the sem being taken made it fail 
             */
             OS_count_sem_table[sem_id].current_value ++;
             ret_val =  OS_SEM_FAILURE;
             break;
          }
      }

      ret_val =  OS_SEM_TIMEOUT;

      /* 
      ** Unlock
      */
      pthread_mutex_unlock(&OS_count_sem_table_mut);

      return(ret_val);
   }
#else
   /*
   ** The Linux version can use the sem_timedwait call
   */
   int32 OS_CountSemTimedWait ( uint32 sem_id, uint32 msecs )
   {
       int32           ret_val = OS_SUCCESS;
       struct timespec ts;
       int             sem_stat;

       if( (sem_id >= OS_MAX_COUNT_SEMAPHORES) || (OS_count_sem_table[sem_id].free == TRUE) )
       {
          return OS_ERR_INVALID_ID;
       }

       /*
       ** Lock
       */
       pthread_mutex_lock(&OS_count_sem_table_mut);  

       OS_count_sem_table[sem_id].current_value --;

       /*
       ** Compute an absolute time for the delay
       */
       ret_val = OS_CompAbsDelayTime( msecs , &ts) ;

       /*
       ** If the pend gets interrupted by a system call, then re-enter the wait
       */
       do
       {
           sem_stat = sem_timedwait(&OS_count_sem_table[sem_id].id, &ts);
       } while ( sem_stat == -1 && errno == EINTR );

       if ( sem_stat == 0 )
       {
            ret_val = OS_SUCCESS;
       }
       else if ( sem_stat == -1 && errno == ETIMEDOUT )
       {
          /* Time out */
          OS_count_sem_table[sem_id].current_value++;
          ret_val = OS_SEM_TIMEOUT;
       }
       else
       {
          /*
          ** something besides the sem being taken made it fail
          */
          OS_count_sem_table[sem_id].current_value ++;
       
          ret_val = OS_SEM_FAILURE;

       }
    
       /* 
       ** Unlock
       */
       pthread_mutex_unlock(&OS_count_sem_table_mut);

       return(ret_val);
   }
#endif
/*--------------------------------------------------------------------------------------
    Name: OS_CountSemGetIdByName

    Purpose: This function tries to find a counting sem Id given the name of a count_sem
             The id is returned through sem_id

    Returns: OS_INVALID_POINTER is semid or sem_name are NULL pointers
             OS_ERR_NAME_TOO_LONG if the name given is to long to have been stored
             OS_ERR_NAME_NOT_FOUND if the name was not found in the table
             OS_SUCCESS if success
             
---------------------------------------------------------------------------------------*/
int32 OS_CountSemGetIdByName (uint32 *sem_id, const char *sem_name)
{
    uint32 i;

    if (sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /* 
    ** a name too long wouldn't have been allowed in the first place
    ** so we definitely won't find a name too long
    */    
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    for (i = 0; i < OS_MAX_COUNT_SEMAPHORES; i++)
    {
        if (OS_count_sem_table[i].free != TRUE &&
                (strcmp (OS_count_sem_table[i].name , (char*) sem_name) == 0))
        {
            *sem_id = i;
            return OS_SUCCESS;
        }
    }
    /* 
    ** The name was not found in the table,
    ** or it was, and the sem_id isn't valid anymore 
    */
    return OS_ERR_NAME_NOT_FOUND;
    
}/* end OS_CountSemGetIdByName */
/*---------------------------------------------------------------------------------------
    Name: OS_CountSemGetInfo

    Purpose: This function will pass back a pointer to structure that contains 
             all of the relevant info( name and creator) about the specified counting
             semaphore.
             
    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid semaphore 
             OS_INVALID_POINTER if the count_prop pointer is null
             OS_SUCCESS if success
---------------------------------------------------------------------------------------*/

int32 OS_CountSemGetInfo (uint32 sem_id, OS_count_sem_prop_t *count_prop)  
{
    /* 
    ** Check to see that the id given is valid 
    */
    if (sem_id >= OS_MAX_COUNT_SEMAPHORES || OS_count_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    if (count_prop == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /*
    ** Lock
    */
    pthread_mutex_lock(&OS_count_sem_table_mut);  
    
    /* put the info into the stucture */
    count_prop -> value = OS_count_sem_table[sem_id].current_value;
    
    count_prop -> creator =    OS_count_sem_table[sem_id].creator;
    strcpy(count_prop-> name, OS_count_sem_table[sem_id].name);
   
    /*
    ** Unlock
    */ 
    pthread_mutex_unlock(&OS_count_sem_table_mut);

    return OS_SUCCESS;
    
} /* end OS_CountSemGetInfo */
/****************************************************************************************
                                  MUTEX API
****************************************************************************************/

/*---------------------------------------------------------------------------------------
    Name: OS_MutSemCreate

    Purpose: Creates a mutex semaphore initially full.

    Returns: OS_INVALID_POINTER if sem_id or sem_name are NULL
             OS_ERR_NAME_TOO_LONG if the sem_name is too long to be stored
             OS_ERR_NO_FREE_IDS if there are no more free mutex Ids
             OS_ERR_NAME_TAKEN if there is already a mutex with the same name
             OS_SEM_FAILURE if the OS call failed
             OS_SUCCESS if success
    
    Notes: the options parameter is not used in this implementation

---------------------------------------------------------------------------------------*/
int32 OS_MutSemCreate (uint32 *sem_id, const char *sem_name, uint32 options)
{
    int                 return_code;
    pthread_mutexattr_t mutex_attr ;    
    uint32              possible_semid;
    uint32              i;      

    /* Check Parameters */
    if (sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }
    
    /* we don't want to allow names too long*/
    /* if truncated, two names might be the same */
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    pthread_mutex_lock(&OS_mut_sem_table_mut);  

    for (possible_semid = 0; possible_semid < OS_MAX_MUTEXES; possible_semid++)
    {
        if (OS_mut_sem_table[possible_semid].free == TRUE)    
            break;
    }
    
    if( (possible_semid == OS_MAX_MUTEXES) ||
        (OS_mut_sem_table[possible_semid].free != TRUE) )
    {
        pthread_mutex_unlock(&OS_mut_sem_table_mut);
        return OS_ERR_NO_FREE_IDS;
    }

    /* Check to see if the name is already taken */

    for (i = 0; i < OS_MAX_MUTEXES; i++)
    {
        if ((OS_mut_sem_table[i].free == FALSE) &&
                strcmp ((char*) sem_name, OS_mut_sem_table[i].name) == 0)
        {
            pthread_mutex_unlock(&OS_mut_sem_table_mut);
            return OS_ERR_NAME_TAKEN;
        }
    }

    /* Set the free flag to false to make sure no other task grabs it */

    OS_mut_sem_table[possible_semid].free = FALSE;
    pthread_mutex_unlock(&OS_mut_sem_table_mut);

    /* 
          ** initialize the attribute with default values 
          */
    return_code = pthread_mutexattr_init(&mutex_attr); 
#ifndef __CYGWIN32__ 
    if ( return_code != 0 )
    {
        /* Since the call failed, set free back to true */
        pthread_mutex_lock(&OS_mut_sem_table_mut);
        OS_mut_sem_table[possible_semid].free = TRUE;
        pthread_mutex_unlock(&OS_mut_sem_table_mut);

       printf("Error: Mutex could not be created. pthread_mutexattr_init failed ID = %lu\n",possible_semid);
       return OS_SEM_FAILURE;
    }

#ifdef _MAC_OS_	
    /*
    ** setprotocol and settype are unsupported by linux or cygwin
    */  
    return_code = pthread_mutexattr_setprotocol(&mutex_attr,PTHREAD_PRIO_INHERIT) ;
    if ( return_code != 0 )
    {
        /* Since the call failed, set free back to true */
        pthread_mutex_lock(&OS_mut_sem_table_mut);
        OS_mut_sem_table[possible_semid].free = TRUE;
        pthread_mutex_unlock(&OS_mut_sem_table_mut);

       printf("Error: Mutex could not be created. pthread_mutexattr_setprotocol failed ID = %lu\n",possible_semid);
       return OS_SEM_FAILURE;    
    }	
    /*
    **  Set the mutex type to RECURSIVE so a thread can do nested locks
    */
    return_code = pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    if ( return_code != 0 )
    {
        /* Since the call failed, set free back to true */
        pthread_mutex_lock(&OS_mut_sem_table_mut);
        OS_mut_sem_table[possible_semid].free = TRUE;
        pthread_mutex_unlock(&OS_mut_sem_table_mut);

       printf("Error: Mutex could not be created. pthread_mutexattr_settype failed ID = %lu\n",possible_semid);
       return OS_SEM_FAILURE;   
    }
#endif /* _MAC_OS_ */
#endif /* __CYGWIN32__  */
  
    /* 
    ** create the mutex 
    ** upon successful initialization, the state of the mutex becomes initialized and ulocked 
    */
    return_code =  pthread_mutex_init((pthread_mutex_t *) &OS_mut_sem_table[possible_semid].id,&mutex_attr); 
    if ( return_code != 0 )
    {
        /* Since the call failed, set free back to true */
        pthread_mutex_lock(&OS_mut_sem_table_mut);
        OS_mut_sem_table[possible_semid].free = TRUE;
        pthread_mutex_unlock(&OS_mut_sem_table_mut);

       printf("Error: Mutex could not be created. ID = %lu\n",possible_semid);
       return OS_SEM_FAILURE;
    }
    else
    {
       /*
       ** Mark mutex as initialized
       */
       /* printf("Mutex created, mutex_id = %d \n" ,possible_semid) ;*/
       
       *sem_id = possible_semid;
    
       pthread_mutex_lock(&OS_mut_sem_table_mut);  

       strcpy(OS_mut_sem_table[*sem_id].name, (char*) sem_name);
       OS_mut_sem_table[*sem_id].free = FALSE;
       OS_mut_sem_table[*sem_id].creator = OS_FindCreator();
    
       pthread_mutex_unlock(&OS_mut_sem_table_mut);

       return OS_SUCCESS;
    }

}/* end OS_MutexSemCreate */

/*--------------------------------------------------------------------------------------
     Name: OS_MutSemDelete

    Purpose: Deletes the specified Mutex Semaphore.
    
    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid mutex
             OS_SEM_FAILURE if the OS call failed
             OS_SUCCESS if success

    Notes: The mutex must be full to take it, so we have to check for fullness

---------------------------------------------------------------------------------------*/

int32 OS_MutSemDelete (uint32 sem_id)
{
    int status=-1;

    /* Check to see if this sem_id is valid   */
    if (sem_id >= OS_MAX_MUTEXES || OS_mut_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    status = pthread_mutex_destroy( &(OS_mut_sem_table[sem_id].id)); /* 0 = success */   
    
    if( status != 0)
    {
        return OS_SEM_FAILURE;
    }
    /* Delete its presence in the table */
   
    pthread_mutex_lock(&OS_mut_sem_table_mut);  

    OS_mut_sem_table[sem_id].free = TRUE;
    strcpy(OS_mut_sem_table[sem_id].name , "");
    OS_mut_sem_table[sem_id].creator = UNINITIALIZED;
    
    pthread_mutex_unlock(&OS_mut_sem_table_mut);
    
    return OS_SUCCESS;

}/* end OS_MutSemDelete */

/*---------------------------------------------------------------------------------------
    Name: OS_MutSemGive

    Purpose: The function releases the mutex object referenced by sem_id.The 
             manner in which a mutex is released is dependent upon the mutex's type 
             attribute.  If there are threads blocked on the mutex object referenced by 
             mutex when this function is called, resulting in the mutex becoming 
             available, the scheduling policy shall determine which thread shall 
             acquire the mutex.

    Returns: OS_SUCCESS if success
             OS_SEM_FAILURE if the semaphore was not previously  initialized 
             OS_ERR_INVALID_ID if the id passed in is not a valid mutex

---------------------------------------------------------------------------------------*/

int32 OS_MutSemGive ( uint32 sem_id )
{
    uint32 ret_val ;

    /* Check Parameters */

    if(sem_id >= OS_MAX_MUTEXES || OS_mut_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    /*
    ** Unlock the mutex
    */
    if ( OS_mut_sem_table[sem_id].nested_value > 0 )
    {
       OS_mut_sem_table[sem_id].nested_value--;
       return OS_SUCCESS;
    }    
    else if(pthread_mutex_unlock(&(OS_mut_sem_table[sem_id].id)))
    {
        ret_val = OS_SEM_FAILURE ;
    }
    else
    {
        ret_val = OS_SUCCESS ;
    }
    
    return ret_val;
} /* end OS_MutSemGive */

/*---------------------------------------------------------------------------------------
    Name: OS_MutSemTake

    Purpose: The mutex object referenced by sem_id shall be locked by calling this
             function. If the mutex is already locked, the calling thread shall
             block until the mutex becomes available. This operation shall return
             with the mutex object referenced by mutex in the locked state with the              
             calling thread as its owner.

    Returns: OS_SUCCESS if success
             OS_SEM_FAILURE if the semaphore was not previously initialized or is 
             not in the array of semaphores defined by the system
             OS_ERR_INVALID_ID the id passed in is not a valid mutex
---------------------------------------------------------------------------------------*/
int32 OS_MutSemTake ( uint32 sem_id )
{
    int status;

    /* 
    ** Check Parameters
    */  
    if(sem_id >= OS_MAX_MUTEXES || OS_mut_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }
 
    /*
    ** Lock the mutex - unlike the sem calls, the pthread mutex call
    ** should not be interrupted by a signal
    */
    status = pthread_mutex_lock(&(OS_mut_sem_table[sem_id].id));
    if( status == EINVAL )
    {
      return OS_SEM_FAILURE ;
    }
    else if ( status == EDEADLK )
    {
       /*
       ** This status code happens if the task already has the mutex locked. In this case we do not want
       ** to return an error code.
       */
       OS_mut_sem_table[sem_id].nested_value++;

       return OS_SUCCESS ;
    }
    else
    {
      return OS_SUCCESS;
    }

}
/*--------------------------------------------------------------------------------------
    Name: OS_MutSemGetIdByName

    Purpose: This function tries to find a mutex sem Id given the name of a mut_sem
             The id is returned through sem_id

    Returns: OS_INVALID_POINTER is semid or sem_name are NULL pointers
             OS_ERR_NAME_TOO_LONG if the name given is to long to have been stored
             OS_ERR_NAME_NOT_FOUND if the name was not found in the table
             OS_SUCCESS if success
             
---------------------------------------------------------------------------------------*/
int32 OS_MutSemGetIdByName (uint32 *sem_id, const char *sem_name)
{
    uint32 i;

    if(sem_id == NULL || sem_name == NULL)
    {
        return OS_INVALID_POINTER;
    }

    /* a name too long wouldn't have been allowed in the first place
     * so we definitely won't find a name too long*/
    
    if (strlen(sem_name) >= OS_MAX_API_NAME)
    {
        return OS_ERR_NAME_TOO_LONG;
    }

    for (i = 0; i < OS_MAX_MUTEXES; i++)
    {
        if ((OS_mut_sem_table[i].free != TRUE) &&
           (strcmp (OS_mut_sem_table[i].name, (char*) sem_name) == 0) )
        {
            *sem_id = i;
            return OS_SUCCESS;
        }
    }
    
    /* The name was not found in the table,
     *  or it was, and the sem_id isn't valid anymore */
    return OS_ERR_NAME_NOT_FOUND;

}/* end OS_MutSemGetIdByName */

/*---------------------------------------------------------------------------------------
    Name: OS_MutSemGetInfo

    Purpose: This function will pass back a pointer to structure that contains 
             all of the relevant info( name and creator) about the specified mutex
             semaphore.
             
    Returns: OS_ERR_INVALID_ID if the id passed in is not a valid semaphore 
             OS_INVALID_POINTER if the mut_prop pointer is null
             OS_SUCCESS if success
---------------------------------------------------------------------------------------*/

int32 OS_MutSemGetInfo (uint32 sem_id, OS_mut_sem_prop_t *mut_prop)  
{
    /* Check to see that the id given is valid */
    
    if (sem_id >= OS_MAX_MUTEXES || OS_mut_sem_table[sem_id].free == TRUE)
    {
        return OS_ERR_INVALID_ID;
    }

    if (mut_prop == NULL)
    {
        return OS_INVALID_POINTER;
    }
    
    /* put the info into the stucture */    
    
    pthread_mutex_lock(&OS_mut_sem_table_mut);  

    mut_prop -> creator =   OS_mut_sem_table[sem_id].creator;
    strcpy(mut_prop-> name, OS_mut_sem_table[sem_id].name);

    pthread_mutex_unlock(&OS_mut_sem_table_mut);
    
    return OS_SUCCESS;
    
} /* end OS_BinSemGetInfo */


/****************************************************************************************
                                    INT API
****************************************************************************************/

/*---------------------------------------------------------------------------------------
** Name: OS_IntAttachHandler
**
** Purpose:
**      The call associates a specified C routine to a specified interrupt   
**      number.Upon occurring of the InterruptNumber , the InerruptHandler 
**      routine will be called and passed the parameter. 
**
** Assumptions and Notes:
**
** Parameters:
**      InterruptNumber : The Interrupt Number that will cause the start of
**                        the ISR
**      InerruptHandler : The ISR associatd with this interrupt
**      parameter :The parameter that is passed to the ISR
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**     OS_SUCCESS on success
**     OS_INVALID_POINTER if the Interrupt handler pointer is NULL
---------------------------------------------------------------------------------------*/
int32 OS_IntAttachHandler  (uint32 InterruptNumber, osal_task_entry InterruptHandler, int32 parameter)
{
    if (InterruptHandler == NULL)
    {
        return OS_INVALID_POINTER;
    }
    return(OS_ERR_NOT_IMPLEMENTED);
}

/*---------------------------------------------------------------------------------------
** Name: OS_IntUnlock
** Purpose:
**      Enable the interrupts. 
**
** Assumptions and Notes:
**
** Parameters:
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/
int32 OS_IntUnlock (int32 IntLevel)
{
    return(OS_ERR_NOT_IMPLEMENTED);
}

/*---------------------------------------------------------------------------------------
** Name: OS_Intlock
** Purpose:
**      Disable the interrupts. 
**
** Assumptions and Notes:
**
** Parameters:
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/
int32 OS_IntLock ( void ) 
{
    return(OS_ERR_NOT_IMPLEMENTED) ;
}
/*---------------------------------------------------------------------------------------
** Name: OS_IntEnable
** Purpose:
**      Enables interrupts through Level 
**
** Assumptions and Notes:
**
** Parameters:
**              Level - the interrupts to enable
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/
int32 OS_IntEnable(int32 Level)
{
    return(OS_ERR_NOT_IMPLEMENTED);
}

/*---------------------------------------------------------------------------------------
** Name: OS_IntDisable
** Purpose:
**      Disables interrupts through Level 
**
** Assumptions and Notes:
**
** Parameters:
**              Level - the interrupts to disable
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/

int32 OS_IntDisable(int32 Level)
{
    return(OS_ERR_NOT_IMPLEMENTED);
}

/*---------------------------------------------------------------------------------------
   Name: OS_HeapGetInfo

   Purpose: Return current info on the heap

   Parameters:

---------------------------------------------------------------------------------------*/
int32 OS_HeapGetInfo       (OS_heap_prop_t *heap_prop)
{
    if (heap_prop == NULL)
    {
        return OS_INVALID_POINTER;
    }
    /*
    ** Not implemented yet
    */
    return (OS_ERR_NOT_IMPLEMENTED);
}
/*---------------------------------------------------------------------------------------
** Name: OS_Tick2Micros
**
** Purpose:
** This function returns the duration of a system tick in micro seconds.
**
** Assumptions and Notes:
**
** Parameters: None
**
** Global Inputs: None
**
** Global Outputs: None
**
**
**
** Return Values: duration of a system tick in micro seconds
---------------------------------------------------------------------------------------*/
int32 OS_Tick2Micros (void)
{
   /*
   ** In an OSX or Linux system, what is the number of ticks per second ?
   ** and how do we find out ?
   */
   /*return(10000);*/
   return((1/(CLOCKS_PER_SEC))*1000);
}



/*---------------------------------------------------------------------------------------
** Name: OS_Milli2Ticks
**
** Purpose:
** This function accepts a time interval in milli_seconds, as an input and 
** returns the tick equivalent  for this time period. The tick value is 
**  rounded up.
**
** Assumptions and Notes:
**
** Parameters:
**      milli_seconds : the time interval ,in milli second , to be translated
**                      to ticks
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: the number of ticks rounded up.
---------------------------------------------------------------------------------------*/

int32 OS_Milli2Ticks(uint32 milli_seconds)
{
    uint32 num_of_ticks,tick_duration_usec ;

    tick_duration_usec = OS_Tick2Micros() ;

    num_of_ticks = 
        ( (milli_seconds * 1000) + tick_duration_usec -1 ) / tick_duration_usec ;

    return(num_of_ticks) ; 

}
/*---------------------------------------------------------------------------------------
 * Name: OS_GetLocalTime
 * 
 * Purpose: This functions get the local time of the machine its on
 * ------------------------------------------------------------------------------------*/

int32 OS_GetLocalTime(OS_time_t *time_struct)
{
/* the code that is #if 0'd out below is what we actually want to use. However 
 * clock_gettime doesn;t seem to be implemented in linux right now. The code that is
 * being used instead will return the clock of the system its in. Hopefully that will
 * be good enough for this port for now. */
    
    struct timeval tv;
    int Status;
    int32 ReturnCode;

    if (time_struct == NULL)
    {
       return OS_INVALID_POINTER;
    }

    Status = gettimeofday(&tv, NULL);
    time_struct-> seconds = tv.tv_sec;
    time_struct-> microsecs = tv.tv_usec;
    
    if (Status == 0)
    {
        ReturnCode = OS_SUCCESS;
    }
    else
    {
        ReturnCode = OS_ERROR;
    }
    
    return ReturnCode;

#if 0  
   int status;
   struct  timespec  time;

   if (time_struct == NULL)
      return OS_INVALID_POINTER;
   
    /*status = clock_gettime(CLOCK_REALTIME, &time);*/
    if (status != 0)
        return OS_ERROR;

   time_struct -> seconds = time.tv_sec;
   time_struct -> microsecs = time.tv_nsec / 1000;

    return OS_SUCCESS;

#endif


}/* end OS_GetLocalTime */


/*---------------------------------------------------------------------------------------
 * Name: OS_SetLocalTime
 * 
 * Purpose: This functions set the local time of the machine its on
 * ------------------------------------------------------------------------------------*/

int32 OS_SetLocalTime(OS_time_t *time_struct)
{
    struct timeval tv;
    int Status;
    int32 ReturnCode;

    if (time_struct == NULL)
    {
      return OS_INVALID_POINTER;
    }

    tv.tv_sec = time_struct -> seconds;
    tv.tv_usec = time_struct -> microsecs;

    Status = settimeofday(&tv, NULL);

    if (Status == 0)
    {
        ReturnCode = OS_SUCCESS;
    }
    else
    {
        ReturnCode = OS_ERROR;
    }
    
    return ReturnCode;

} /*end OS_SetLocalTime */

/*---------------------------------------------------------------------------------------
** Name: OS_SetMask
** Purpose:
**      Set the masking register to mask and unmask interrupts 
**
** Assumptions and Notes:
**
** Parameters:
**      MaskSetting :the value to be written into the mask register
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/
int32 OS_SetMask ( uint32 MaskSetting ) 
{
    return(OS_SUCCESS) ;
}

/*--------------------------------------------------------------------------------------
** Name: OS_GetMask
** Purpose:
**      Read and report the setting of the cpu mask register.
**
** Assumptions and Notes:
**
** Parameters:
**      MaskSettingPtr : pointer to a location where the function store the
**                               reading of the cpu mask register.
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: 
**      OS_SUCCESS
---------------------------------------------------------------------------------------*/
int32 OS_GetMask ( uint32 * MaskSettingPtr ) 
{
    return(OS_SUCCESS) ;
}
/*--------------------------------------------------------------------------------------
 * uint32 FindCreator
 * purpose: Finds the creator of the calling thread
---------------------------------------------------------------------------------------*/
uint32 OS_FindCreator(void)
{
    pthread_t    pthread_id;
    uint32 i;  
   
    pthread_id = pthread_self();
    /* 
    ** Get PTHREAD Id
    */
    for (i = 0; i < OS_MAX_TASKS; i++)
    {
        if (pthread_equal(pthread_id, OS_task_table[i].id) != 0 )
        {
            break;
        }
    }

    return i;
}

/*---------------------------------------------------------------------------------------
** Name: OS_CompAbsDelayTime
**
** Purpose:
** This function accept time interval, msecs, as an input and 
** computes the absolute time at which this time interval will expire. 
** The absolute time is programmed into a struct.
**
** Assumptions and Notes:
**
** Parameters:
**
** Global Inputs: None
**
** Global Outputs: None
**
**
** Return Values: OS_SUCCESS, 
---------------------------------------------------------------------------------------*/
uint32  OS_CompAbsDelayTime( uint32 msecs, struct timespec * tm)
{
#ifdef _MAC_OS_
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tm->tv_sec = tv.tv_sec;
    tm->tv_nsec = tv.tv_usec * 1000;
#else
    clock_gettime( CLOCK_REALTIME,  tm ); 
#endif

    /* add the delay to the current time */
    tm->tv_sec  += (time_t) (msecs / 1000) ;
    /* convert residue ( msecs )  to nanoseconds */
    tm->tv_nsec +=  (msecs % 1000) * 1000000L ;
    
    if(tm->tv_nsec  >= 1000000000L )
    {
        tm->tv_nsec -= 1000000000L ;
        tm->tv_sec ++ ;
    }
    
    return(OS_SUCCESS) ;    
}
/* ---------------------------------------------------------------------------
 * Name: OS_printf 
 * 
 * Purpose: This function abstracts out the printf type statements. This is 
 *          useful for using OS- specific thats that will allow non-polled
 *          print statements for the real time systems. 
 ---------------------------------------------------------------------------*/
void OS_printf( const char *String, ...)
{
    va_list     ptr;
    char msg_buffer [OS_BUFFER_SIZE];
    
    va_start(ptr,String);
    vsnprintf(&msg_buffer[0], (size_t)OS_BUFFER_SIZE, String, ptr);
    va_end(ptr);
    
    msg_buffer[OS_BUFFER_SIZE -1] = '\0';
    printf("%s", &msg_buffer[0]);
    
}/* end OS_printf*/

/*---------------------------------------------------------------------------------------
 *  Name: OS_GetErrorName()
---------------------------------------------------------------------------------------*/
int32 OS_GetErrorName(int32 error_num, os_err_name_t * err_name)
{
    os_err_name_t local_name;
    uint32 return_code;

    return_code = OS_SUCCESS;

    switch (error_num)
    {
        case OS_SUCCESS:
            strcpy(local_name,"OS_SUCCESS"); break;
        case OS_ERROR:
            strcpy(local_name,"OS_ERROR"); break;
        case OS_INVALID_POINTER:
            strcpy(local_name,"OS_INVALID_POINTER"); break;
        case OS_ERROR_ADDRESS_MISALIGNED:
            strcpy(local_name,"OS_ADDRESS_MISALIGNED"); break;
        case OS_ERROR_TIMEOUT:
            strcpy(local_name,"OS_ERROR_TIMEOUT"); break;
        case OS_INVALID_INT_NUM:
            strcpy(local_name,"OS_INVALID_INT_NUM"); break;
        case OS_SEM_FAILURE:
            strcpy(local_name,"OS_SEM_FAILURE"); break;
        case OS_SEM_TIMEOUT:
            strcpy(local_name,"OS_SEM_TIMEOUT"); break;
        case OS_QUEUE_EMPTY:
            strcpy(local_name,"OS_QUEUE_EMPTY"); break;
        case OS_QUEUE_FULL:
            strcpy(local_name,"OS_QUEUE_FULL"); break;
        case OS_QUEUE_TIMEOUT:
            strcpy(local_name,"OS_QUEUE_TIMEOUT"); break;
        case OS_QUEUE_INVALID_SIZE:
            strcpy(local_name,"OS_QUEUE_INVALID_SIZE"); break;
        case OS_QUEUE_ID_ERROR:
            strcpy(local_name,"OS_QUEUE_ID_ERROR"); break;
        case OS_ERR_NAME_TOO_LONG:
            strcpy(local_name,"OS_ERR_NAME_TOO_LONG"); break;
        case OS_ERR_NO_FREE_IDS:
            strcpy(local_name,"OS_ERR_NO_FREE_IDS"); break;
        case OS_ERR_NAME_TAKEN:
            strcpy(local_name,"OS_ERR_NAME_TAKEN"); break;
        case OS_ERR_INVALID_ID:
            strcpy(local_name,"OS_ERR_INVALID_ID"); break;
        case OS_ERR_NAME_NOT_FOUND:
            strcpy(local_name,"OS_ERR_NAME_NOT_FOUND"); break;
        case OS_ERR_SEM_NOT_FULL:
            strcpy(local_name,"OS_ERR_SEM_NOT_FULL"); break;
        case OS_ERR_INVALID_PRIORITY:
            strcpy(local_name,"OS_ERR_INVALID_PRIORITY"); break;

        default: strcpy(local_name,"ERROR_UNKNOWN");
                 return_code = OS_ERROR;
    }

    strcpy((char*) err_name, local_name);

    return return_code;
}
/*----------------------------------------------------------------------------
 * Name: OS_PriorityRemap
 *
 * Purpose: Remaps the OSAL priority into one that is viable for this OS
----------------------------------------------------------------------------*/

int32 OS_PriorityRemap(uint32 InputPri)
{
    int OutputPri;
    int pmax = sched_get_priority_max( SCHED_FIFO );
    int pmin = sched_get_priority_min( SCHED_FIFO );
    int prange = abs((pmax - pmin)  +1);
    int numbins, offset;
    int IsMinNegative = 0;
    int MinNegOffset = 0;
    int IsMaxNegative = 0;
    int MaxNegOffset = 0;
    int InputRev;

    /* If an end point is negative, adjust the range upward so that there is no negative */
    if (pmin < 0)
    {
        IsMinNegative = 1;
        MinNegOffset = -pmin;
        pmin += MinNegOffset;
        pmax += MinNegOffset;
    } 

    if (pmax < 0)
    {
        IsMaxNegative = 1;
        MaxNegOffset = -pmax;
        pmin += MaxNegOffset;
        pmax += MaxNegOffset;
    }

    /* calculate the number of 'bins' to map the OSAL priorities into.
     * Since the Underlying OS will have AT MOST as many priority levels as
     * the OSAL (256 values), then we will be pigeon-holing a larger range 
     * (OSAL priorities) into a small range (OS priorities), or the ranges are
     * equal, which is OK too.
     */

    numbins = MAX_PRIORITY/prange;

    /* If we are more than half way to making a new bin, add another one */
   if (MAX_PRIORITY % prange > prange/2)
   {
      numbins++;
   } 
    

    /* Since the OSAL priorities have 0 as the highest and 255 as the lowest,
     * we need to reverse this so that the numerically higher number is a higher priority
     * to work with the OS's
     */

     InputRev = MAX_PRIORITY - InputPri;
       
     /* calculate the offset from the min value */
     offset = InputRev / numbins ;
          
     OutputPri = pmin + offset ;  

     /* take care of extraneous cases at ends, if they occur. */
     if (OutputPri > pmax)
     {
         OutputPri = pmax;
     }

     if ( OutputPri < pmin)
     {
            OutputPri = pmin;
     }

     /* if an end point was negative, shift it back */
     if (IsMinNegative == 1)
     {
         OutputPri -= MinNegOffset;
     }

     if (IsMaxNegative == 1)
     {
         OutputPri -= MaxNegOffset;
     }


    return OutputPri;
}/*end OS_PriortyRemap*/

/* ---------------------------------------------------------------------------
 * Name: OS_ThreadKillHandler
 * 
 * Purpose: This function allows for a task to be deleted when OS_TaskDelete
 * is called  
----------------------------------------------------------------------------*/

void    OS_ThreadKillHandler(int sig)
{
    pthread_exit(NULL);

}/*end OS_ThreadKillHandler */


/*
**
**   Name: OS_FPUExcSetMask
**
**   Purpose: This function sets the FPU exception mask
**
**   Notes: The exception environment is local to each task Therefore this must be
**          called for each task that that wants to do floating point and catch exceptions.
*/
int32 OS_FPUExcSetMask(uint32 mask)
{
    /*
    ** Not implemented in linux.
    */
    return(OS_SUCCESS);
}

/*
**
**   Name: OS_FPUExcGetMask
**
**   Purpose: This function gets the FPU exception mask
**
**   Notes: The exception environment is local to each task Therefore this must be
**          called for each task that that wants to do floating point and catch exceptions.
*/
int32 OS_FPUExcGetMask(uint32 *mask)
{
    /*
    ** Not implemented in linux.
    */
    return(OS_SUCCESS);
}

